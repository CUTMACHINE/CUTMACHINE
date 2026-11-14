#include "ResolveExport.h"

#include "Cli.h"
#include "ClipAnimation.h"
#include "ClipFraming.h"
#include "ClipSpeed.h"
#include "Edl.h"
#include "Json.h"
#include "Multicam.h"
#include "ProjectStorage.h"
#include "Subtitles.h"
#include "TransitionRender.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>
#include <set>

namespace {

using mcp_json::Value;

// Decimals past this many digits are dropped when a fraction becomes the
// text of a JSON number: a millionth of a pixel, of a zoom or of a
// percentage of speed is below anything Resolve stores.
constexpr int kDecimalDigits = 6;

ResolveXmlFraction Reduced(__int128 numerator, __int128 denominator) {
    if (denominator < 0) {
        numerator = -numerator;
        denominator = -denominator;
    }
    __int128 a = numerator < 0 ? -numerator : numerator;
    __int128 b = denominator;
    while (b != 0) {
        const __int128 t = a % b;
        a = b;
        b = t;
    }
    if (a == 0) a = 1;
    return {static_cast<int64_t>(numerator / a),
            static_cast<int64_t>(denominator / a)};
}

Value Decimal(const ResolveXmlFraction& value) {
    return Value::MakeRawNumber(
        FormatResolveXmlFraction(value, kDecimalDigits));
}

Value Decimal(const EffectParamValue& value) {
    return Decimal(ResolveXmlFraction{value.num, value.den});
}

bool IsBlack(const std::string& color) {
    std::string lower;
    for (char c : color)
        lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return lower == "#000000";
}

// Fills `exported.losses` by kind, in the order kinds are first met.
class Losses {
public:
    explicit Losses(std::vector<ResolveSendLoss>& losses) : losses_(losses) {}

    void Add(const std::string& what, const std::string& detail,
             const Ulid& id) {
        auto found = index_.find(what);
        if (found == index_.end()) {
            found = index_.emplace(what, losses_.size()).first;
            losses_.push_back({what, detail, {}});
        }
        std::vector<Ulid>& ids = losses_[found->second].ids;
        if (std::find(ids.begin(), ids.end(), id) == ids.end())
            ids.push_back(id);
    }

private:
    std::vector<ResolveSendLoss>& losses_;
    std::map<std::string, size_t> index_;
};

const LibraryMedia* FindMedia(const Document& document, const Ulid& id) {
    for (const LibraryMedia& media : document.library)
        if (media.id == id) return &media;
    return nullptr;
}

std::vector<const DocumentTrack*> TracksOfKind(const Document& document,
                                               const std::string& kind) {
    std::vector<const DocumentTrack*> tracks;
    for (const DocumentTrack& track : document.sequence.tracks)
        if (track.kind == kind) tracks.push_back(&track);
    std::stable_sort(tracks.begin(), tracks.end(),
                     [](const DocumentTrack* left, const DocumentTrack* right) {
                         return left->index < right->index;
                     });
    return tracks;
}

// A clip's picture as Resolve's Transform reads it under one scaling: the
// static key of ResolveXmlMotionFor, whose centre Resolve reads as
// Pan = x * W and Tilt = -y * H (measured 28 October
// and 4 November), and whose scale is Zoom * 100.
bool FramingFor(const Document& document, const DocumentClip& still,
                ResolveScaling scaling, ResolveSendFraming& framing,
                std::string& error) {
    std::vector<ResolveXmlMotionKey> keys;
    if (!ResolveXmlMotionFor(document, still, 0, 1, scaling, keys, error))
        return false;
    if (keys.empty()) {
        error = "clip '" + still.id + "' gave no framing";
        return false;
    }
    const ResolveXmlMotionKey& key = keys.front();
    const __int128 width = document.sequence.width;
    const __int128 height = document.sequence.height;
    framing.zoom = Reduced(key.scale_percent.num,
                           static_cast<__int128>(key.scale_percent.den) * 100);
    framing.pan = Reduced(static_cast<__int128>(key.center_x.num) * width,
                          key.center_x.den);
    framing.tilt = Reduced(-static_cast<__int128>(key.center_y.num) * height,
                           key.center_y.den);
    return true;
}

bool SendClip(const Document& document, const ResolveSendOptions& options,
              const DocumentClip& clip, bool audio, int32_t track,
              Losses& losses, ResolveTimelineClip& sent, std::string& error) {
    const MediaRate& sequenceRate = document.sequence.frame_rate;
    const LibraryMedia* media = FindMedia(document, clip.source_id);
    if (media == nullptr) {
        error = "clip '" + clip.id + "' references media '" + clip.source_id +
                "' that is not in the library";
        return false;
    }
    // Resolve gives an audio file the project's cadence (music.wav read
    // FPS 25 in a 25 project, and startFrame 50 opened it at 2 s): its
    // frames are the sequence's.
    const MediaRate unit = media->has_video ? media->rate : sequenceRate;
    if (unit.num <= 0 || unit.den <= 0) {
        error = "media '" + media->filename + "' has no usable frame rate";
        return false;
    }
    int64_t record = 0;
    int64_t duration = 0;
    if (!ExactFrameCount(clip.timeline_in, sequenceRate, record) ||
        !ExactFrameCount(clip.duration, sequenceRate, duration)) {
        error = "clip '" + clip.id + "' on '" + media->filename +
                "' does not sit on whole sequence frames; sending it would "
                "move the cut";
        return false;
    }
    if (duration <= 0) {
        error = "clip '" + clip.id + "' is shorter than one sequence frame";
        return false;
    }
    // The record length in source frames at speed 1: what AppendToTimeline
    // is given before SetSpeed, which keeps the length (see the header).
    int64_t span = 0;
    if (!ExactFrameCount(clip.duration, unit, span) || span <= 0) {
        error = "clip '" + clip.id + "' on '" + media->filename + "' lasts " +
                std::to_string(duration) +
                " sequence frames, which is not a whole number of frames of "
                "its source at " +
                std::to_string(unit.num) + "/" + std::to_string(unit.den) +
                ": Resolve conforms a clip by its source length, and would "
                "move the next cut";
        return false;
    }
    int64_t first = 0;
    if (!ExactFrameCount(clip.source_in, unit, first)) {
        error = "clip '" + clip.id + "' on '" + media->filename +
                "' does not start on a whole frame of its source at " +
                std::to_string(unit.num) + "/" + std::to_string(unit.den);
        return false;
    }

    ResolveXmlFraction speed{clip.speed.num, clip.speed.den};
    if (ClipHasSpeedRamp(clip)) {
        // A ramp has no API: its mean speed reads the same source in the
        // same length, which is what keeps the cuts and the frames at both
        // ends right.
        const RationalTime consumed = ClipSourceSpan(clip);
        speed =
            Reduced(static_cast<__int128>(consumed.value) * clip.duration.rate,
                    static_cast<__int128>(consumed.rate) * clip.duration.value);
        losses.Add("speed_ramps",
                   "a speed ramp has no API: the clip plays its mean speed, "
                   "the same source in the same length",
                   clip.id);
    }
    if (speed.num < 0 || speed.den <= 0) {
        error = "clip '" + clip.id + "' has a speed Resolve cannot take";
        return false;
    }
    const int64_t mediaFrames =
        media->duration.rate > 0 ? media->duration.to_frames(unit.num, unit.den)
                                 : -1;
    int64_t start = first;
    int64_t end = first + span;
    if (speed.num == 0) {
        // SetSpeed(0) holds the last frame of the range it is given.
        start = first - span + 1;
        end = first + 1;
        if (start < 0) {
            error = "clip '" + clip.id + "' on '" + media->filename +
                    "' holds source frame " + std::to_string(first) + " for " +
                    std::to_string(span) +
                    " frames; Resolve holds the last frame of the range a "
                    "clip is placed with, and that range would start before "
                    "the media: hold the frame in Resolve instead (Retime "
                    "Controls, Freeze Frame)";
            return false;
        }
    } else if (mediaFrames >= 0 && end > mediaFrames) {
        error = "clip '" + clip.id + "' on '" + media->filename +
                "' is a slow motion that Resolve has to place at speed 1 "
                "first, over source frames " +
                std::to_string(first) + " to " + std::to_string(end) +
                ", past the media's " + std::to_string(mediaFrames) +
                ": shorten it, start it earlier, or slow it down in Resolve";
        return false;
    }

    sent.clip_id = clip.id;
    sent.audio = audio;
    sent.track = track;
    sent.path = media->path;
    sent.filename = media->filename;
    sent.start_frame = start;
    sent.end_frame = end;
    sent.record_frame = record;
    sent.duration = duration;
    sent.speed = Reduced(speed.num, speed.den);
    sent.enabled = clip.enabled;

    if (audio) {
        sent.gain_db = clip.audio_gain_db;
        const auto fade = [&](const RationalTime& length, int64_t& frames) {
            if (length.value <= 0 || length.rate <= 0) return;
            if (ExactFrameCount(length, sequenceRate, frames)) return;
            // Half up: a fade is a level, not a cut.
            const __int128 numerator =
                static_cast<__int128>(length.value) * sequenceRate.num;
            const __int128 denominator =
                static_cast<__int128>(length.rate) * sequenceRate.den;
            frames = static_cast<int64_t>((2 * numerator + denominator) /
                                          (2 * denominator));
            losses.Add("fades_rounded",
                       "Resolve counts a fade in whole frames: rounded to the "
                       "nearest",
                       clip.id);
        };
        fade(clip.audio_fade_in, sent.fade_in);
        fade(clip.audio_fade_out, sent.fade_out);
        return true;
    }

    DocumentClip still = clip;
    if (ClipHasAnimation(clip)) {
        ClipSampledParameters values;
        if (!SampleClipParameters(clip, 0, sequenceRate, values, error)) {
            error = "clip '" + clip.id + "': " + error;
            return false;
        }
        still.framing = values.framing;
        still.opacity = values.opacity;
        still.animation.clear();
        losses.Add("animations",
                   "no API sets a keyframe: the clip keeps its first frame's "
                   "framing and opacity",
                   clip.id);
    }
    const ClipFraming& framing = still.framing;
    if (framing.crop_left.num != 0 || framing.crop_right.num != 0 ||
        framing.crop_top.num != 0 || framing.crop_bottom.num != 0) {
        still.framing.crop_left = {0, 1};
        still.framing.crop_right = {0, 1};
        still.framing.crop_top = {0, 1};
        still.framing.crop_bottom = {0, 1};
        losses.Add("crops",
                   "a crop has no measured equivalent in Resolve: the clip "
                   "is framed whole, by its scale and position",
                   clip.id);
    }
    if (!FramingFor(document, still, ResolveScaling::Fit, sent.fit, error) ||
        !FramingFor(document, still, ResolveScaling::Fill, sent.fill, error))
        return false;
    sent.framed = true;
    sent.opacity = still.opacity;
    if (!clip.effects.empty()) {
        if (options.grade_cdl) {
            std::vector<ColorCdl> cdls;
            ColorGradeSpace space = ColorGradeSpace::Display;
            std::string cdlError;
            if (ClipColorCdls(document, {clip.id}, cdls, space, cdlError) &&
                cdls.size() == 1) {
                sent.graded = true;
                sent.cdl = cdls.front();
            } else {
                losses.Add("grades_without_cdl",
                           "the grade holds a stage with no CDL form "
                           "(export_cdl says which): not sent",
                           clip.id);
            }
        } else {
            losses.Add("grades",
                       "grades travel only on request, as a CDL "
                       "(grade: \"cdl\")",
                       clip.id);
        }
    }
    if (ClipHasBlur(clip))
        losses.Add("blurs", "a blur has no API: blur the shot in Resolve",
                   clip.id);
    if (ClipHasMask(clip))
        losses.Add("masks", "a mask has no API: mask the shot in Resolve",
                   clip.id);
    return true;
}

}  // namespace

bool ExactFrameCount(const RationalTime& time, const MediaRate& rate,
                     int64_t& frames) {
    if (time.rate <= 0 || rate.num <= 0 || rate.den <= 0) return false;
    const __int128 numerator = static_cast<__int128>(time.value) * rate.num;
    const __int128 denominator = static_cast<__int128>(time.rate) * rate.den;
    if (denominator == 0 || numerator % denominator != 0) return false;
    const __int128 quotient = numerator / denominator;
    if (quotient > INT64_MAX || quotient < INT64_MIN) return false;
    frames = static_cast<int64_t>(quotient);
    return true;
}

bool BuildResolveTimelineExport(const Document& document,
                                ResolveTimelineExport& exported,
                                std::string& error,
                                const ResolveSendOptions& options) {
    const DocumentSequence& sequence = document.sequence;
    const MediaRate& sequenceRate = sequence.frame_rate;
    if (sequenceRate.num <= 0 || sequenceRate.den <= 0) {
        error = "the timeline has no usable frame rate";
        return false;
    }
    ResolveTimelineExport built;
    built.name = sequence.name;
    built.frame_rate = sequenceRate;
    built.width = sequence.width;
    built.height = sequence.height;
    built.grade_cdl = options.grade_cdl;
    built.cdl_node = options.cdl_node;
    Losses losses(built.losses);

    const std::vector<const DocumentTrack*> videoTracks =
        TracksOfKind(document, "video");
    const std::vector<const DocumentTrack*> audioTracks =
        TracksOfKind(document, "audio");
    const bool anySolo =
        std::any_of(audioTracks.begin(), audioTracks.end(),
                    [](const DocumentTrack* track) { return track->solo; });
    for (size_t index = 0; index < videoTracks.size(); ++index)
        built.tracks.push_back({false, static_cast<int32_t>(index + 1),
                                videoTracks[index]->visible, false});
    if (videoTracks.empty()) built.tracks.push_back({false, 1, true, false});
    for (size_t index = 0; index < audioTracks.size(); ++index) {
        const DocumentTrack& track = *audioTracks[index];
        // The voice: every clip a rush's own sound. Music, a voice-over or
        // a sound effect is a file without pictures, and stays stereo.
        bool mono = !track.clips.empty();
        for (const DocumentClip& clip : track.clips) {
            const LibraryMedia* media = FindMedia(document, clip.source_id);
            if (media == nullptr || !media->has_video) mono = false;
        }
        built.tracks.push_back({true, static_cast<int32_t>(index + 1),
                                !track.muted && (!anySolo || track.solo),
                                mono});
    }
    if (audioTracks.empty()) built.tracks.push_back({true, 1, true, false});

    const std::set<Ulid> hidden = MulticamHiddenClipIds(sequence);
    std::map<Ulid, size_t> clipIndex;
    for (const bool audio : {false, true}) {
        const std::vector<const DocumentTrack*>& tracks =
            audio ? audioTracks : videoTracks;
        for (size_t index = 0; index < tracks.size(); ++index) {
            std::vector<const DocumentClip*> ordered;
            for (const DocumentClip& clip : tracks[index]->clips)
                ordered.push_back(&clip);
            std::stable_sort(
                ordered.begin(), ordered.end(),
                [](const DocumentClip* left, const DocumentClip* right) {
                    return left->timeline_in < right->timeline_in;
                });
            for (const DocumentClip* clip : ordered) {
                if (!audio && hidden.count(clip->id) != 0) {
                    ++built.multicam_angles_not_sent;
                    continue;
                }
                // MOTION-2026-09 M4 -- a motion composition has no file in
                // the Media Pool. Its place stays empty: render it as a
                // plate (render_motion_video, alpha) and lay it in Resolve.
                if (IsMotionClip(*clip)) {
                    losses.Add("motion_clips",
                               "a motion composition has no file in "
                               "Resolve: its place stays empty (render it "
                               "with render_motion_video and lay the plate)",
                               clip->id);
                    continue;
                }
                ResolveTimelineClip sent;
                if (!SendClip(document, options, *clip, audio,
                              static_cast<int32_t>(index + 1), losses, sent,
                              error))
                    return false;
                clipIndex[clip->id] = built.clips.size();
                built.clips.push_back(std::move(sent));
            }
        }
    }
    if (built.clips.empty()) {
        error = "the timeline has no clip to send";
        return false;
    }

    // Links: the clips of one A/V separation that both travel.
    std::map<Ulid, std::vector<size_t>> groups;
    for (const auto& [id, index] : clipIndex) {
        for (const DocumentTrack& track : sequence.tracks) {
            for (const DocumentClip& clip : track.clips) {
                if (clip.id == id && !clip.link_group_id.empty())
                    groups[clip.link_group_id].push_back(index);
            }
        }
    }
    int32_t nextGroup = 0;
    for (auto& [groupId, members] : groups) {
        if (members.size() < 2) continue;
        std::sort(members.begin(), members.end());
        for (size_t member : members)
            built.clips[member].link_group = nextGroup;
        ++nextGroup;
    }

    for (const DocumentTransition& transition : sequence.transitions) {
        TransitionSpan span;
        std::string spanError;
        if (!ResolveTransitionSpan(sequence, transition, span, spanError)) {
            error = "transition '" + transition.id + "' " + spanError;
            return false;
        }
        const auto left = clipIndex.find(transition.left_clip_id);
        const auto right = clipIndex.find(transition.right_clip_id);
        if (TransitionIsColorDip(transition)) {
            // The veil reads no handle (TransitionRender.h): the outgoing
            // clip fades out over its last `pre` frames, the incoming one
            // in over its first `post`, on black.
            if (!IsBlack(transition.color))
                losses.Add("dip_colors",
                           "Resolve's fades dip to black: a dip through "
                           "another colour arrives black",
                           transition.id);
            bool placed = false;
            if (left != clipIndex.end()) {
                ResolveTimelineClip& clip = built.clips[left->second];
                clip.fade_out = std::max(clip.fade_out, span.pre_frames);
                placed = true;
            }
            if (right != clipIndex.end()) {
                ResolveTimelineClip& clip = built.clips[right->second];
                clip.fade_in = std::max(clip.fade_in, span.post_frames);
                placed = true;
            }
            if (!placed)
                losses.Add("transitions", "touches no clip that was sent",
                           transition.id);
            continue;
        }
        if (left == clipIndex.end() || right == clipIndex.end()) {
            losses.Add("transitions", "joins a clip that was not sent",
                       transition.id);
            continue;
        }
        ResolveSendTransition sent;
        sent.transition_id = transition.id;
        sent.audio = TransitionIsCrossFade(transition);
        sent.clip = left->second;
        sent.alignment =
            transition.alignment == TransitionAlignment::Center       ? "center"
            : transition.alignment == TransitionAlignment::StartAtCut ? "right"
                                                                      : "left";
        sent.type = sent.audio ? "Cross Fade +3 dB" : "Cross Dissolve";
        sent.category = sent.audio ? "audio" : "simple";
        sent.duration = span.frames();
        sent.record_start = span.first_frame();
        sent.record_end = span.end_frame();
        built.transitions.push_back(std::move(sent));
    }

    std::vector<const DocumentMarker*> markers;
    for (const DocumentMarker& marker : sequence.markers)
        markers.push_back(&marker);
    std::stable_sort(
        markers.begin(), markers.end(),
        [](const DocumentMarker* left, const DocumentMarker* right) {
            return left->time < right->time;
        });
    std::map<int64_t, size_t> markerAt;
    for (const DocumentMarker* marker : markers) {
        int64_t frame = 0;
        int64_t length = 0;
        if (!ExactFrameCount(marker->time, sequenceRate, frame) ||
            !ExactFrameCount(marker->duration, sequenceRate, length) ||
            frame < 0) {
            error =
                "marker '" + marker->id + "' is not on a whole sequence frame";
            return false;
        }
        std::string word;
        bool exact = false;
        std::string colorError;
        if (!EdlResolveMarkerColorWord(marker->color, word, exact,
                                       colorError)) {
            error = "marker '" + marker->id + "': " + colorError;
            return false;
        }
        if (!exact)
            losses.Add("marker_colors",
                       "Resolve has sixteen marker colours: the nearest one",
                       marker->id);
        const auto existing = markerAt.find(frame);
        if (existing != markerAt.end()) {
            ResolveSendMarker& merged = built.markers[existing->second];
            merged.marker_ids.push_back(marker->id);
            merged.name += " / " + marker->name;
            merged.duration = std::max(merged.duration, length);
            losses.Add("markers_merged",
                       "Resolve holds one marker per frame: their names are "
                       "joined on the first one's",
                       marker->id);
            continue;
        }
        ResolveSendMarker sent;
        sent.marker_ids = {marker->id};
        sent.frame = frame;
        sent.duration = std::max<int64_t>(length, 1);
        sent.color = word;
        sent.name = marker->name;
        // The category says what the marker is for, past the default one.
        if (marker->category != DocumentMarker{}.category)
            sent.note = marker->category;
        markerAt[frame] = built.markers.size();
        built.markers.push_back(std::move(sent));
    }

    int64_t end = 0;
    for (const ResolveTimelineClip& clip : built.clips)
        end = std::max(end, clip.record_frame + clip.duration);
    for (const DocumentTrack* track : TracksOfKind(document, "caption")) {
        if (!track->visible) continue;
        const std::vector<SubtitleCue> cues =
            CaptionTrackCues(document, *track);
        if (cues.empty()) continue;
        if (built.subtitle_cues != 0) {
            losses.Add("caption_tracks",
                       "AppendToTimeline stacks every subtitle on S1: one "
                       "caption track travels, the first",
                       track->id);
            continue;
        }
        std::string srtError;
        if (!FormatSrt(cues, built.subtitles, srtError)) {
            error = "caption track '" + track->id + "': " + srtError;
            return false;
        }
        built.subtitle_cues = cues.size();
        losses.Add("caption_style",
                   "the cartons take the timeline's subtitle style in "
                   "Resolve; the house style is grafted by the .drt",
                   track->id);
        for (const SubtitleCue& cue : cues) {
            const RationalTime cueEnd = cue.timeline_in.add(cue.duration);
            int64_t frames = 0;
            if (!ExactFrameCount(cueEnd, sequenceRate, frames))
                frames =
                    cueEnd.to_frames(sequenceRate.num, sequenceRate.den) + 1;
            end = std::max(end, frames);
        }
    }
    built.duration = end;

    if (!sequence.look_lut.digest.empty())
        losses.Add("look_lut",
                   "the timeline's node graph has no node and the API creates "
                   "none: lay the look through a colour group in Resolve",
                   sequence.id);

    exported = std::move(built);
    error.clear();
    return true;
}

std::string SerializeResolveTimelineExport(
    const ResolveTimelineExport& exported) {
    Value root = Value::MakeObject();
    root.Set("ok", Value::MakeBool(true));
    root.Set("schema", Value::MakeString("cutmachine.resolve-timeline.v3"));
    root.Set("name", Value::MakeString(exported.name));
    Value rate = Value::MakeObject();
    rate.Set("num", Value::MakeInt(exported.frame_rate.num));
    rate.Set("den", Value::MakeInt(exported.frame_rate.den));
    root.Set("frame_rate", std::move(rate));
    root.Set("width", Value::MakeInt(exported.width));
    root.Set("height", Value::MakeInt(exported.height));
    root.Set("duration", Value::MakeInt(exported.duration));
    root.Set("grade", Value::MakeString(exported.grade_cdl ? "cdl" : "none"));

    Value tracks = Value::MakeArray();
    for (const ResolveSendTrack& track : exported.tracks) {
        Value entry = Value::MakeObject();
        entry.Set("kind", Value::MakeString(track.audio ? "audio" : "video"));
        entry.Set("index", Value::MakeInt(track.index));
        entry.Set("enabled", Value::MakeBool(track.enabled));
        if (track.audio) entry.Set("mono", Value::MakeBool(track.mono));
        tracks.Push(std::move(entry));
    }
    root.Set("tracks", std::move(tracks));

    const auto framing = [](const ResolveSendFraming& value) {
        Value entry = Value::MakeObject();
        entry.Set("zoom", Decimal(value.zoom));
        entry.Set("pan", Decimal(value.pan));
        entry.Set("tilt", Decimal(value.tilt));
        return entry;
    };
    Value clips = Value::MakeArray();
    for (const ResolveTimelineClip& clip : exported.clips) {
        Value entry = Value::MakeObject();
        entry.Set("clip_id", Value::MakeString(clip.clip_id));
        entry.Set("kind", Value::MakeString(clip.audio ? "audio" : "video"));
        entry.Set("track", Value::MakeInt(clip.track));
        entry.Set("path", Value::MakeString(clip.path));
        entry.Set("filename", Value::MakeString(clip.filename));
        entry.Set("start_frame", Value::MakeInt(clip.start_frame));
        entry.Set("end_frame", Value::MakeInt(clip.end_frame));
        entry.Set("record_frame", Value::MakeInt(clip.record_frame));
        entry.Set("duration", Value::MakeInt(clip.duration));
        if (clip.speed.num != clip.speed.den)
            entry.Set(
                "speed_percent",
                Decimal(Reduced(static_cast<__int128>(clip.speed.num) * 100,
                                clip.speed.den)));
        if (!clip.enabled) entry.Set("enabled", Value::MakeBool(false));
        if (clip.link_group >= 0)
            entry.Set("link_group", Value::MakeInt(clip.link_group));
        if (clip.framed) {
            Value transform = Value::MakeObject();
            transform.Set("fit", framing(clip.fit));
            transform.Set("fill", framing(clip.fill));
            entry.Set("transform", std::move(transform));
        }
        if (!clip.audio && clip.opacity.num != clip.opacity.den)
            entry.Set(
                "opacity",
                Decimal(Reduced(static_cast<__int128>(clip.opacity.num) * 100,
                                clip.opacity.den)));
        if (clip.graded) {
            const ResolveCdlMap map =
                ResolveSetCdlMap(clip.cdl, exported.cdl_node);
            Value cdl = Value::MakeObject();
            cdl.Set("NodeIndex", Value::MakeString(map.node_index));
            cdl.Set("Slope", Value::MakeString(map.slope));
            cdl.Set("Offset", Value::MakeString(map.offset));
            cdl.Set("Power", Value::MakeString(map.power));
            cdl.Set("Saturation", Value::MakeString(map.saturation));
            entry.Set("cdl", std::move(cdl));
        }
        if (clip.audio && clip.gain_db.num != 0)
            entry.Set("volume_db", Decimal(clip.gain_db));
        if (clip.fade_in > 0)
            entry.Set("fade_in", Value::MakeInt(clip.fade_in));
        if (clip.fade_out > 0)
            entry.Set("fade_out", Value::MakeInt(clip.fade_out));
        clips.Push(std::move(entry));
    }
    root.Set("clips", std::move(clips));

    Value transitions = Value::MakeArray();
    for (const ResolveSendTransition& transition : exported.transitions) {
        Value entry = Value::MakeObject();
        entry.Set("transition_id", Value::MakeString(transition.transition_id));
        entry.Set("kind",
                  Value::MakeString(transition.audio ? "audio" : "video"));
        entry.Set("clip",
                  Value::MakeInt(static_cast<int64_t>(transition.clip)));
        entry.Set("type", Value::MakeString(transition.type));
        entry.Set("category", Value::MakeString(transition.category));
        entry.Set("alignment", Value::MakeString(transition.alignment));
        entry.Set("duration", Value::MakeInt(transition.duration));
        entry.Set("record_start", Value::MakeInt(transition.record_start));
        entry.Set("record_end", Value::MakeInt(transition.record_end));
        transitions.Push(std::move(entry));
    }
    root.Set("transitions", std::move(transitions));

    Value markers = Value::MakeArray();
    for (const ResolveSendMarker& marker : exported.markers) {
        Value entry = Value::MakeObject();
        Value ids = Value::MakeArray();
        for (const Ulid& id : marker.marker_ids)
            ids.Push(Value::MakeString(id));
        entry.Set("marker_ids", std::move(ids));
        entry.Set("frame", Value::MakeInt(marker.frame));
        entry.Set("duration", Value::MakeInt(marker.duration));
        entry.Set("color", Value::MakeString(marker.color));
        entry.Set("name", Value::MakeString(marker.name));
        entry.Set("note", Value::MakeString(marker.note));
        markers.Push(std::move(entry));
    }
    root.Set("markers", std::move(markers));

    if (exported.subtitle_cues != 0) {
        Value subtitles = Value::MakeObject();
        subtitles.Set("srt", Value::MakeString(exported.subtitles));
        subtitles.Set(
            "cues",
            Value::MakeInt(static_cast<int64_t>(exported.subtitle_cues)));
        root.Set("subtitles", std::move(subtitles));
    }
    root.Set("multicam_angles_not_sent",
             Value::MakeInt(exported.multicam_angles_not_sent));
    Value losses = Value::MakeArray();
    for (const ResolveSendLoss& loss : exported.losses) {
        Value entry = Value::MakeObject();
        entry.Set("what", Value::MakeString(loss.what));
        entry.Set("detail", Value::MakeString(loss.detail));
        entry.Set("count",
                  Value::MakeInt(static_cast<int64_t>(loss.ids.size())));
        Value ids = Value::MakeArray();
        for (const Ulid& id : loss.ids) ids.Push(Value::MakeString(id));
        entry.Set("ids", std::move(ids));
        losses.Push(std::move(entry));
    }
    root.Set("losses", std::move(losses));
    return root.Dump() + "\n";
}

void AbsoluteLibraryPaths(Document& document,
                          const std::string& packageDirectory) {
    const std::filesystem::path base(packageDirectory);
    for (LibraryMedia& media : document.library) {
        std::filesystem::path stored(media.path);
        if (stored.is_relative()) stored = base / stored;
        std::error_code pathError;
        const std::filesystem::path resolved =
            std::filesystem::weakly_canonical(stored, pathError);
        if (!pathError) media.path = resolved.string();
    }
}

int ExportResolveTimelineCommand(const std::string& projectPath,
                                 std::string& output,
                                 const std::string& timelineId,
                                 const ResolveSendOptions& options) {
    Project project;
    std::string error;
    if (!LoadStoredProject(projectPath, project, error)) {
        return FailCliCommand("InvalidDocument", error, output);
    }
    const Ulid selectedTimeline =
        timelineId.empty() ? project.active_timeline_id : timelineId;
    if (!project.FindTimeline(selectedTimeline)) {
        return FailCliCommand("UnknownSequence",
                              "unknown timeline_id '" + selectedTimeline + "'",
                              output);
    }
    Document document = project.MakeDocument(selectedTimeline);
    // Media paths are stored relative to the package; the bridge matches them
    // against Resolve's Media Pool, which knows only absolute paths.
    AbsoluteLibraryPaths(
        document,
        std::filesystem::absolute(projectPath).parent_path().string());
    ResolveTimelineExport exported;
    if (!BuildResolveTimelineExport(document, exported, error, options)) {
        return FailCliCommand("InvalidExport", error, output);
    }
    output = SerializeResolveTimelineExport(exported);
    return 0;
}
