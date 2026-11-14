#include "ResolveXmlExport.h"

#include "Cli.h"
#include "ClipAnimation.h"
#include "ClipFraming.h"
#include "ClipSpeed.h"
#include "Json.h"
#include "Multicam.h"
#include "ProjectStorage.h"
#include "ResolveExport.h"
#include "TransitionRender.h"

#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <set>
#include <sstream>
#include <stdexcept>

namespace {

// XML text, not JSON: the five predefined entities and nothing else. Rush
// names come from a filesystem and a bin name may hold an ampersand.
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
                escaped += character;
                break;
        }
    }
    return escaped;
}

// A pathurl is a URI: everything outside the unreserved set is percent
// encoded, the separators kept. Resolve ignores this field when it matches a
// media (see the header), but the file still has to be readable by anything
// else that opens it, and a rush folder named "Réalisation 2026" is common.
std::string PathUrl(const std::string& path) {
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

// The integral timebase xmeml wants. A non-integral cadence is refused
// rather than rounded: 23.976 written as 24 moves every frame of the
// montage, and this format has no room for the exact fraction.
bool IntegralTimebase(const MediaRate& rate, int64_t& timebase) {
    if (rate.num <= 0 || rate.den <= 0) return false;
    if (rate.num % rate.den != 0) return false;
    timebase = rate.num / rate.den;
    return true;
}

std::string Indent(size_t depth) { return std::string(depth * 4, ' '); }

// FI6 -- docs/roadmap/FIABILITE-2026-10.md. An agent chooses this path:
// publishing an XML must never truncate the project, a rush, or a previous
// delivery. Stage the complete bytes beside the destination and install
// them without replacement, including on volumes without hard links.
bool WriteNewXml(const std::string& path, const std::string& xml,
                 std::string& errorName, std::string& error) {
    const std::string temporary =
        (std::filesystem::path(path).parent_path() /
         (".cutmachine-xml-" + GenerateUlid() + ".tmp"))
            .string();
    const int fd = ::open(temporary.c_str(),
                          O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
    if (fd < 0) {
        errorName = "IoError";
        error = "cannot prepare '" + path + "': " + std::strerror(errno);
        return false;
    }
    int failure = 0;
    size_t written = 0;
    while (written < xml.size()) {
        const ssize_t count =
            ::write(fd, xml.data() + written, xml.size() - written);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            failure = count < 0 ? errno : EIO;
            break;
        }
        written += static_cast<size_t>(count);
    }
    if (failure == 0 && ::fsync(fd) != 0) {
        failure = errno;
    }
    if (::close(fd) != 0 && failure == 0) {
        failure = errno;
    }
    if (failure == 0) {
#ifdef __APPLE__
        if (::renameatx_np(AT_FDCWD, temporary.c_str(), AT_FDCWD, path.c_str(),
                           RENAME_EXCL) != 0) {
            failure = errno;
        }
#else
        // Keep the pure-C++ writer usable on POSIX test hosts too. Unlike
        // macOS's exclusive rename, this requires hard links on the volume.
        if (::link(temporary.c_str(), path.c_str()) != 0) {
            failure = errno;
        }
#endif
        // An exFAT or FAT volume (an external SSD as sold) supports neither
        // an exclusive rename nor a hard link: ENOTSUP, measured on exFAT.
        // There the destination is reserved by an exclusive create, which
        // such a volume supports, and the staged bytes replace that empty
        // reservation, never a file someone else made.
        if (failure == ENOTSUP || failure == EPERM) {
            const int reserved = ::open(
                path.c_str(),
                O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0666);
            if (reserved < 0) {
                failure = errno;
            } else {
                ::close(reserved);
                failure = 0;
                if (::rename(temporary.c_str(), path.c_str()) != 0) {
                    failure = errno;
                    ::unlink(path.c_str());
                }
            }
        }
    }
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    if (failure != 0) {
        errorName = failure == EEXIST ? "OutputExists" : "IoError";
        error = "cannot publish '" + path + "': " + std::strerror(failure);
        return false;
    }
    return true;
}

void WriteRate(std::ostringstream& output, int64_t timebase, size_t depth) {
    output << Indent(depth) << "<rate>\n"
           << Indent(depth + 1) << "<timebase>" << timebase << "</timebase>\n"
           << Indent(depth + 1) << "<ntsc>FALSE</ntsc>\n"
           << Indent(depth) << "</rate>\n";
}

}  // namespace

std::string FormatSpeedPercent(const EffectParamValue& speed) {
    if (speed.num <= 0 || speed.den <= 0) return "100";
    const int64_t numerator = static_cast<int64_t>(speed.num) * 100;
    const int64_t denominator = speed.den;
    std::string text = std::to_string(numerator / denominator);
    int64_t remainder = numerator % denominator;
    if (remainder == 0) return text;
    std::string fraction;
    for (int digit = 0; digit < kSpeedPercentDigits && remainder != 0;
         ++digit) {
        remainder *= 10;
        fraction += static_cast<char>('0' + remainder / denominator);
        remainder %= denominator;
    }
    while (!fraction.empty() && fraction.back() == '0') fraction.pop_back();
    if (fraction.empty()) return text;
    return text + "." + fraction;
}

std::string FormatResolveXmlFraction(const ResolveXmlFraction& value,
                                     int digits) {
    if (value.den == 0) return "0";
    __int128 numerator = value.num;
    __int128 denominator = value.den;
    if (denominator < 0) {
        numerator = -numerator;
        denominator = -denominator;
    }
    const bool negative = numerator < 0;
    if (negative) numerator = -numerator;
    std::string text =
        std::to_string(static_cast<int64_t>(numerator / denominator));
    __int128 remainder = numerator % denominator;
    std::string fraction;
    for (int digit = 0; digit < digits && remainder != 0; ++digit) {
        remainder *= 10;
        fraction +=
            static_cast<char>('0' + static_cast<int>(remainder / denominator));
        remainder %= denominator;
    }
    while (!fraction.empty() && fraction.back() == '0') fraction.pop_back();
    if (!fraction.empty()) text += "." + fraction;
    const bool zero = text == "0";
    return negative && !zero ? "-" + text : text;
}

bool ParseResolveScaling(const std::string& text, ResolveScaling& scaling) {
    if (text == "fit") {
        scaling = ResolveScaling::Fit;
        return true;
    }
    if (text == "fill") {
        scaling = ResolveScaling::Fill;
        return true;
    }
    return false;
}

bool ResolveXmlMotionFor(const Document& document, const DocumentClip& clip,
                         int64_t mediaIn, int64_t durationFrames,
                         ResolveScaling scaling,
                         std::vector<ResolveXmlMotionKey>& keys,
                         std::string& error) {
    keys.clear();
    if (scaling == ResolveScaling::Undeclared) {
        error = "clip '" + clip.id +
                "' is framed, and the zoom Resolve needs depends on how the "
                "project places a picture whose size is not the timeline's "
                "(Project Settings, Image Scaling, Mismatched resolution "
                "files, or the timeline's own setting): declare it, "
                "resolve_scaling 'fit' for \"Scale entire image to fit\" or "
                "'fill' for \"Scale full frame with crop\". A 16:9 rush in a "
                "9:16 timeline is three times larger in one than in the other";
        return false;
    }
    int64_t displayedWidth = 0;
    int64_t displayedHeight = 0;
    if (!ClipDisplayedSize(document, clip, displayedWidth, displayedHeight,
                           error)) {
        error = "clip '" + clip.id + "': " + error;
        return false;
    }
    const int64_t width = document.sequence.width;
    const int64_t height = document.sequence.height;
    if (displayedWidth <= 0 || displayedHeight <= 0 || width <= 0 ||
        height <= 0) {
        error = "clip '" + clip.id + "' has no picture size to frame";
        return false;
    }
    // Resolve's Zoom 1.0 is the picture its scaling places: fitted or
    // filling. A framing in the same mode maps as is; across modes it
    // carries the ratio of the two scales, max(W/Wd, H/Hd) over
    // min(W/Wd, H/Hd) -- both over the same Wd*Hd, i.e. of W*Hd and H*Wd --
    // or its inverse.
    const __int128 byWidth = static_cast<__int128>(width) * displayedHeight;
    const __int128 byHeight = static_cast<__int128>(height) * displayedWidth;
    const __int128 larger = std::max(byWidth, byHeight);
    const __int128 smaller = std::min(byWidth, byHeight);
    // R9, #240 -- Resolve normalizes Basic Motion's centre against the
    // picture at Zoom 1.0, before the clip's zoom, rather than against the
    // sequence frame. A portrait fitted in a landscape sequence therefore
    // needs a larger horizontal fraction for the same pixel translation.
    const bool scalesByWidth = scaling == ResolveScaling::Fit
                                   ? byWidth <= byHeight
                                   : byWidth >= byHeight;
    const int64_t baseNumerator = scalesByWidth ? width : height;
    const int64_t baseDenominator =
        scalesByWidth ? displayedWidth : displayedHeight;
    const auto reduce = [](__int128 numerator, __int128 denominator) {
        __int128 a = numerator < 0 ? -numerator : numerator;
        __int128 b = denominator;
        while (b != 0) {
            const __int128 t = a % b;
            a = b;
            b = t;
        }
        if (a == 0) a = 1;
        return ResolveXmlFraction{static_cast<int64_t>(numerator / a),
                                  static_cast<int64_t>(denominator / a)};
    };
    const auto keyAt = [&](const ClipFraming& framing, int64_t frame) {
        ResolveXmlMotionKey key;
        key.when = mediaIn + frame;
        __int128 numerator = static_cast<__int128>(framing.scale.num) * 100;
        __int128 denominator = framing.scale.den;
        const bool fills = framing.mode == FramingMode::Fill;
        if (fills && scaling == ResolveScaling::Fit) {
            numerator *= larger;
            denominator *= smaller;
        } else if (!fills && scaling == ResolveScaling::Fill) {
            numerator *= smaller;
            denominator *= larger;
        }
        key.scale_percent = reduce(numerator, denominator);
        key.center_x = reduce(static_cast<__int128>(framing.position_x.num) *
                                  width * baseDenominator,
                              static_cast<__int128>(framing.position_x.den) *
                                  displayedWidth * baseNumerator);
        key.center_y = reduce(static_cast<__int128>(framing.position_y.num) *
                                  height * baseDenominator,
                              static_cast<__int128>(framing.position_y.den) *
                                  displayedHeight * baseNumerator);
        return key;
    };
    const auto crops = [](const ClipFraming& framing) {
        return framing.crop_left.num != 0 || framing.crop_right.num != 0 ||
               framing.crop_top.num != 0 || framing.crop_bottom.num != 0;
    };
    if (crops(clip.framing)) {
        error = "clip '" + clip.id +
                "' crops its source; Resolve's Basic Motion has no crop this "
                "route has measured, and a framing sent without it would show "
                "what the crop removed: frame it by scale and position only, "
                "or crop it in Resolve";
        return false;
    }
    if (!ClipHasAnimation(clip)) {
        keys.push_back(keyAt(clip.framing, 0));
        return true;
    }
    std::vector<ResolveXmlMotionKey> sampled;
    sampled.reserve(static_cast<size_t>(durationFrames));
    for (int64_t frame = 0; frame < durationFrames; ++frame) {
        ClipSampledParameters values;
        std::string sampleError;
        if (!SampleClipParameters(clip, frame, document.sequence.frame_rate,
                                  values, sampleError)) {
            error = "clip '" + clip.id + "' at frame " + std::to_string(frame) +
                    ": " + sampleError;
            return false;
        }
        sampled.push_back(keyAt(values.framing, frame));
    }
    // Resolve interpolates linearly between keys, so a key is kept on every
    // frame that differs from a neighbour: holds stay holds, and a curve is
    // the list of values the engine sampled.
    const auto same = [](const ResolveXmlMotionKey& left,
                         const ResolveXmlMotionKey& right) {
        const auto equal = [](const ResolveXmlFraction& a,
                              const ResolveXmlFraction& b) {
            return static_cast<__int128>(a.num) * b.den ==
                   static_cast<__int128>(b.num) * a.den;
        };
        return equal(left.scale_percent, right.scale_percent) &&
               equal(left.center_x, right.center_x) &&
               equal(left.center_y, right.center_y);
    };
    for (size_t index = 0; index < sampled.size(); ++index) {
        const bool edge = index == 0 || index + 1 == sampled.size();
        const bool changes =
            (index > 0 && !same(sampled[index], sampled[index - 1])) ||
            (index + 1 < sampled.size() &&
             !same(sampled[index], sampled[index + 1]));
        if (edge || changes) keys.push_back(sampled[index]);
    }
    return true;
}

bool BuildResolveXmlTimeline(const Document& document,
                             ResolveXmlTimeline& timeline, std::string& error,
                             ResolveScaling scaling) {
    ResolveXmlTimeline built;
    built.name = document.sequence.name;
    built.frame_rate = document.sequence.frame_rate;
    built.width = document.sequence.width;
    built.height = document.sequence.height;

    int64_t sequenceTimebase = 0;
    if (!IntegralTimebase(document.sequence.frame_rate, sequenceTimebase)) {
        error =
            "the sequence cadence is not a whole number of frames per "
            "second; the FCP7 XML has no exact fraction for it, and rounding "
            "it would move every cut";
        return false;
    }

    // Video layers are numbered from the bottom track up, as in
    // ResolveExport.h, so the two return trips name the same layer.
    std::vector<const DocumentTrack*> videoTracks;
    for (const DocumentTrack& track : document.sequence.tracks) {
        if (track.kind == "video") videoTracks.push_back(&track);
        if (track.kind == "audio") ++built.audio_tracks_not_sent;
    }
    std::stable_sort(videoTracks.begin(), videoTracks.end(),
                     [](const DocumentTrack* left, const DocumentTrack* right) {
                         return left->index < right->index;
                     });
    built.video_layers = static_cast<int32_t>(videoTracks.size());

    // F1.5 -- an inactive multicam angle is not in the picture (Multicam.h),
    // so it is not written. Unlike the AppendToTimeline trip, this one
    // carries no sound at all, so there is no sound left behind by hiding it
    // -- only a count, reported rather than dropped silently.
    const std::set<Ulid> multicamHidden =
        MulticamHiddenClipIds(document.sequence);

    const auto mediaIndex = [&](const DocumentClip& clip, size_t& index,
                                std::string& why) {
        const auto found =
            std::find_if(document.library.begin(), document.library.end(),
                         [&](const LibraryMedia& entry) {
                             return entry.id == clip.source_id;
                         });
        if (found == document.library.end()) {
            why = "clip '" + clip.id + "' references media '" + clip.source_id +
                  "' that is not in the library";
            return false;
        }
        if (found->rate.num <= 0 || found->rate.den <= 0) {
            why = "media '" + found->filename + "' has no usable frame rate";
            return false;
        }
        int64_t mediaTimebase = 0;
        if (!IntegralTimebase(found->rate, mediaTimebase)) {
            why = "media '" + found->filename +
                  "' has a cadence that is not a whole number of frames per "
                  "second; the FCP7 XML cannot name it exactly";
            return false;
        }
        for (size_t existing = 0; existing < built.media.size(); ++existing) {
            if (built.media[existing].path == found->path) {
                index = existing;
                return true;
            }
        }
        ResolveXmlMedia entry;
        entry.path = found->path;
        entry.filename = found->filename;
        entry.rate = found->rate;
        entry.frames =
            found->duration.to_frames(found->rate.num, found->rate.den);
        entry.start_timecode = kUnknownStartTimecode;
        index = built.media.size();
        built.media.push_back(std::move(entry));
        return true;
    };

    for (size_t layer = 0; layer < videoTracks.size(); ++layer) {
        std::vector<const DocumentClip*> ordered;
        for (const DocumentClip& clip : videoTracks[layer]->clips) {
            if (multicamHidden.count(clip.id) != 0) {
                ++built.multicam_angles_not_sent;
                continue;
            }
            if (IsMotionClip(clip)) {
                error = "clip '" + clip.id +
                        "' shows a motion composition; the Resolve XML does "
                        "not carry motion clips yet (as a video with alpha "
                        "on its track, to validate in Resolve first): remove "
                        "it, or export the timeline instead";
                return false;
            }
            // R9 -- an opacity has no route to Resolve: the XML's Opacity
            // filter was never measured and AppendToTimeline has none. A
            // translucent layer sent opaque is not the document's montage,
            // under a success, so it is refused by name. The framing and
            // its keys travel (Basic Motion, below).
            if (clip.opacity.num != clip.opacity.den ||
                ClipAnimates(clip, ClipAnimatedParameter::Opacity)) {
                error = "clip '" + clip.id +
                        "' is not fully opaque, or animates its opacity; the "
                        "FCP7 XML route does not carry an opacity (not "
                        "measured in Resolve): set it in Resolve instead";
                return false;
            }
            if (!clip.effects.empty()) ++built.grades_not_sent;
            // EF3 -- docs/roadmap/EFFETS-2026-10.md. Nor does a blur: the
            // XML's Gaussian Blur filter was never measured against Resolve.
            // Counted rather than refused, as a grade is: it changes how the
            // shot looks, not the montage the XML exists to carry.
            if (ClipHasBlur(clip)) ++built.blurs_not_sent;
            // PA8 -- docs/roadmap/PARITE-2026-11.md. Nor a mask: counted,
            // for the same reason.
            if (ClipHasMask(clip)) ++built.masks_not_sent;
            ordered.push_back(&clip);
        }
        std::stable_sort(
            ordered.begin(), ordered.end(),
            [](const DocumentClip* left, const DocumentClip* right) {
                return left->timeline_in < right->timeline_in;
            });
        for (const DocumentClip* entry : ordered) {
            const DocumentClip& clip = *entry;
            ResolveXmlClip written;
            written.clip_id = clip.id;
            written.video_layer = static_cast<int32_t>(layer);
            written.speed = clip.speed;
            written.enabled = clip.enabled;
            std::string why;
            if (!mediaIndex(clip, written.media, why)) {
                error = why;
                return false;
            }
            int64_t durationFrames = 0;
            if (!ExactFrameCount(clip.timeline_in, document.sequence.frame_rate,
                                 written.record_in) ||
                !ExactFrameCount(clip.duration, document.sequence.frame_rate,
                                 durationFrames)) {
                error = "clip '" + clip.id +
                        "' does not start and end on whole sequence frames; "
                        "sending it would move the cut";
                return false;
            }
            if (durationFrames <= 0) {
                error =
                    "clip '" + clip.id + "' is shorter than one sequence frame";
                return false;
            }
            written.record_out = written.record_in + durationFrames;
            // The one conversion this ticket adds: the source in-point read
            // in the unit the XML uses. UnscaleBySpeed can overflow on an
            // absurd fraction, which is a refusal like any other.
            if (ClipHasSpeedRamp(clip)) {
                // R3, criterion 4 -- this route writes one retime per clip.
                error = "clip '" + clip.id +
                        "' ramps its speed, which the return to Resolve does "
                        "not carry yet: this XML writes one speed per clip";
                return false;
            }
            if (ClipIsFreeze(clip)) {
                // R4 -- a held frame has no speed the XML's retime can say.
                error = "clip '" + clip.id +
                        "' holds a frame (speed 0/1), which the return to "
                        "Resolve does not carry yet";
                return false;
            }
            RationalTime openedAt;
            try {
                openedAt = UnscaleBySpeed(clip.source_in, clip.speed);
            } catch (const std::exception& exception) {
                error = "clip '" + clip.id +
                        "' has a source in-point its speed cannot express in "
                        "sequence frames: " +
                        exception.what();
                return false;
            }
            if (!ExactFrameCount(openedAt, document.sequence.frame_rate,
                                 written.media_in)) {
                const ResolveXmlMedia& media = built.media[written.media];
                error = "clip '" + clip.id + "' on '" + media.filename +
                        "' opens at a source frame that is not a whole "
                        "sequence frame at speed " +
                        std::to_string(clip.speed.num) + "/" +
                        std::to_string(clip.speed.den) +
                        "; the FCP7 XML writes in and out in sequence frames "
                        "after speed, and there is no frame to write here";
                return false;
            }
            if (written.media_in < 0) {
                error = "clip '" + clip.id + "' opens before its media";
                return false;
            }
            written.media_out = written.media_in + durationFrames;
            // R9 -- the framing, and the keys of an animated one.
            if ((ClipHasFraming(clip) || ClipHasAnimation(clip)) &&
                !ResolveXmlMotionFor(document, clip, written.media_in,
                                     durationFrames, scaling, written.motion,
                                     error)) {
                return false;
            }
            built.duration = std::max(built.duration, written.record_out);
            built.clips.push_back(written);
        }
    }
    if (built.clips.empty()) {
        error = "the active timeline has no video clip to send";
        return false;
    }
    // R6 -- docs/roadmap/REEL-2026-09.md. A sequence look does not travel in
    // the xmeml: R9 measured that Resolve's timeline node graph has no node
    // and its API creates none, so a LUT arrives through a colour group's
    // post-clip graph (SetLUT), which this file does not write. Refused by
    // name rather than sent as a montage without its look -- a wrong picture
    // under a success is exactly what R9's own rule forbids.
    if (!document.sequence.look_lut.digest.empty()) {
        error = "the timeline carries a sequence look LUT ('" +
                document.sequence.look_lut.name +
                "'), which this XML cannot carry: R9 sends a look through a "
                "colour group's post-clip node, not the timeline";
        return false;
    }

    for (const DocumentTransition& transition : document.sequence.transitions) {
        int32_t layer = -1;
        const DocumentTrack* track = nullptr;
        for (size_t index = 0; index < videoTracks.size(); ++index) {
            if (videoTracks[index]->id == transition.track_id) {
                layer = static_cast<int32_t>(index);
                track = videoTracks[index];
            }
        }
        if (track == nullptr) continue;
        // R5 -- docs/roadmap/REEL-2026-09.md. An edge transition names one
        // clip; a dissolve still names two. The span, the alignment split
        // included, is TransitionRender.h's, so an odd-length centred
        // dissolve lands where the engine already says it does instead of
        // being refused or moved half a frame.
        const bool edge = TransitionIsAtTrackEdge(transition);
        TransitionSpan span;
        std::string spanError;
        if (!ResolveTransitionSpan(document.sequence, transition, span,
                                   spanError)) {
            error = "transition '" + transition.id + "' " + spanError;
            return false;
        }
        ResolveXmlTransition written;
        if (TransitionIsColorDip(transition)) {
            // Measured on Resolve 20.3, 19 September 2026: `start-black`
            // and `end-black` pass an import at the edge of a track. A dip
            // in the middle of one is a "Dip to Color Dissolve", a
            // different effectid that no import has been measured with, and
            // a colour other than black has no spelling in the two edge
            // alignments at all. Both are refused by name rather than sent
            // as a dissolve Resolve would render differently (guard rail
            // 5).
            TransitionColor color;
            if (!ParseTransitionColor(transition.color, color, error)) {
                error = "transition '" + transition.id + "' " + error;
                return false;
            }
            if (!edge) {
                error = "transition '" + transition.id +
                        "' dips to a colour between two clips; the XML route "
                        "writes a dip only at the edge of a track (R5/R9)";
                return false;
            }
            if (!(color == TransitionColor{0, 0, 0})) {
                error = "transition '" + transition.id +
                        "' opens or closes on '" + transition.color +
                        "'; the XML route spells an edge fade only as "
                        "'start-black' or 'end-black' (R5/R9)";
                return false;
            }
            written.alignment =
                transition.left_clip_id.empty() ? "start-black" : "end-black";
        } else if (transition.alignment == TransitionAlignment::Center) {
            written.alignment = "center";
        } else if (transition.alignment == TransitionAlignment::StartAtCut) {
            written.alignment = "start";
        } else {
            written.alignment = "end";
        }
        const DocumentClip* named = nullptr;
        for (const DocumentClip& clip : track->clips) {
            if (!transition.left_clip_id.empty() &&
                clip.id == transition.left_clip_id)
                named = &clip;
            if (named == nullptr && !transition.right_clip_id.empty() &&
                clip.id == transition.right_clip_id)
                named = &clip;
        }
        if (named == nullptr) {
            error = "transition '" + transition.id +
                    "' names a clip that is not on its track";
            return false;
        }
        written.id = transition.id;
        written.video_layer = layer;
        written.start = span.first_frame();
        written.end = span.end_frame();
        built.duration = std::max(built.duration, written.end);
        built.transitions.push_back(written);
    }
    std::stable_sort(built.transitions.begin(), built.transitions.end(),
                     [](const ResolveXmlTransition& left,
                        const ResolveXmlTransition& right) {
                         if (left.video_layer != right.video_layer)
                             return left.video_layer < right.video_layer;
                         return left.start < right.start;
                     });

    timeline = std::move(built);
    error.clear();
    return true;
}

std::string WriteResolveXml(const ResolveXmlTimeline& timeline) {
    int64_t sequenceTimebase = 25;
    IntegralTimebase(timeline.frame_rate, sequenceTimebase);

    std::ostringstream output;
    output << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
           << "<!DOCTYPE xmeml>\n"
           // No double hyphen inside: an XML comment may not contain one,
           // and every parser on the way to Resolve refuses the file.
           << "<!-- cutmachine.resolve-xml.v1 (R9). Import it with "
              "resolve_bridge.py, option send-xml: the start timecodes below "
              "are placeholders, and Resolve matches a media by its start "
              "timecode alone. -->\n"
           << "<xmeml version=\"5\">\n"
           << Indent(1) << "<sequence>\n"
           << Indent(2) << "<name>" << EscapeXml(timeline.name) << "</name>\n"
           << Indent(2) << "<duration>" << timeline.duration << "</duration>\n";
    WriteRate(output, sequenceTimebase, 2);
    output << Indent(2) << "<in>-1</in>\n"
           << Indent(2) << "<out>-1</out>\n"
           << Indent(2) << "<timecode>\n"
           << Indent(3) << "<string>01:00:00:00</string>\n"
           << Indent(3) << "<frame>" << 3600 * sequenceTimebase << "</frame>\n"
           << Indent(3) << "<displayformat>NDF</displayformat>\n";
    WriteRate(output, sequenceTimebase, 3);
    output << Indent(2) << "</timecode>\n"
           << Indent(2) << "<media>\n"
           << Indent(3) << "<video>\n";

    // One <file> is defined on its first use and referenced by id after
    // that, as Resolve's own export does; defining it twice would make
    // Resolve resolve the same rush twice.
    std::vector<bool> defined(timeline.media.size(), false);
    for (int32_t layer = 0; layer < std::max(timeline.video_layers, 1);
         ++layer) {
        output << Indent(4) << "<track>\n";
        for (const ResolveXmlClip& clip : timeline.clips) {
            if (clip.video_layer != layer) continue;
            const ResolveXmlMedia& media = timeline.media[clip.media];
            int64_t mediaTimebase = 0;
            IntegralTimebase(media.rate, mediaTimebase);
            output << Indent(5) << "<clipitem id=\"" << EscapeXml(clip.clip_id)
                   << "\">\n"
                   << Indent(6) << "<name>" << EscapeXml(media.filename)
                   << "</name>\n"
                   // The clip's own out point, not the media length: the
                   // media length in this unit is not always a whole number
                   // and the engine does not round. Resolve ignores this
                   // field (measured on 20.3.1).
                   << Indent(6) << "<duration>" << clip.media_out
                   << "</duration>\n";
            WriteRate(output, sequenceTimebase, 6);
            output << Indent(6) << "<start>" << clip.record_in << "</start>\n"
                   << Indent(6) << "<end>" << clip.record_out << "</end>\n"
                   << Indent(6) << "<enabled>"
                   << (clip.enabled ? "TRUE" : "FALSE") << "</enabled>\n"
                   << Indent(6) << "<in>" << clip.media_in << "</in>\n"
                   << Indent(6) << "<out>" << clip.media_out << "</out>\n";
            const std::string fileId = "file-" + std::to_string(clip.media + 1);
            if (defined[clip.media]) {
                output << Indent(6) << "<file id=\"" << fileId << "\"/>\n";
            } else {
                defined[clip.media] = true;
                output << Indent(6) << "<file id=\"" << fileId << "\">\n"
                       << Indent(7) << "<duration>" << media.frames
                       << "</duration>\n";
                WriteRate(output, mediaTimebase, 7);
                output << Indent(7) << "<name>" << EscapeXml(media.filename)
                       << "</name>\n"
                       << Indent(7) << "<pathurl>"
                       << EscapeXml(PathUrl(media.path)) << "</pathurl>\n"
                       << Indent(7) << "<timecode>\n"
                       << Indent(8) << "<string>"
                       << EscapeXml(media.start_timecode) << "</string>\n"
                       << Indent(8) << "<displayformat>NDF</displayformat>\n";
                WriteRate(output, mediaTimebase, 8);
                output << Indent(7) << "</timecode>\n"
                       << Indent(7) << "<media>\n"
                       << Indent(8) << "<video>\n"
                       << Indent(9) << "<samplecharacteristics>\n"
                       << Indent(10) << "<width>" << timeline.width
                       << "</width>\n"
                       << Indent(10) << "<height>" << timeline.height
                       << "</height>\n"
                       << Indent(9) << "</samplecharacteristics>\n"
                       << Indent(8) << "</video>\n"
                       << Indent(7) << "</media>\n"
                       << Indent(6) << "</file>\n";
            }
            output << Indent(6) << "<compositemode>normal</compositemode>\n";
            if (!IsUnitSpeed(clip.speed)) {
                output << Indent(6) << "<filter>\n"
                       << Indent(7) << "<enabled>TRUE</enabled>\n"
                       << Indent(7) << "<start>-1</start>\n"
                       << Indent(7) << "<end>-1</end>\n"
                       << Indent(7) << "<effect>\n"
                       << Indent(8) << "<name>Time Remap</name>\n"
                       << Indent(8) << "<effectid>timeremap</effectid>\n"
                       << Indent(8) << "<effecttype>motion</effecttype>\n"
                       << Indent(8) << "<mediatype>video</mediatype>\n"
                       << Indent(8)
                       << "<effectcategory>motion</effectcategory>\n"
                       << Indent(8) << "<parameter>\n"
                       << Indent(9) << "<name>speed</name>\n"
                       << Indent(9) << "<parameterid>speed</parameterid>\n"
                       << Indent(9) << "<value>"
                       << FormatSpeedPercent(clip.speed) << "</value>\n"
                       << Indent(9) << "<valuemin>-10000</valuemin>\n"
                       << Indent(9) << "<valuemax>10000</valuemax>\n"
                       << Indent(8) << "</parameter>\n"
                       << Indent(8) << "<parameter>\n"
                       << Indent(9) << "<name>reverse</name>\n"
                       << Indent(9) << "<parameterid>reverse</parameterid>\n"
                       << Indent(9) << "<value>FALSE</value>\n"
                       << Indent(8) << "</parameter>\n"
                       << Indent(8) << "<parameter>\n"
                       << Indent(9) << "<name>frameblending</name>\n"
                       << Indent(9)
                       << "<parameterid>frameblending</parameterid>\n"
                       << Indent(9) << "<value>FALSE</value>\n"
                       << Indent(8) << "</parameter>\n"
                       << Indent(8) << "<parameter>\n"
                       << Indent(9) << "<name>variablespeed</name>\n"
                       << Indent(9)
                       << "<parameterid>variablespeed</parameterid>\n"
                       << Indent(9) << "<value>0</value>\n"
                       << Indent(9) << "<valuemin>0</valuemin>\n"
                       << Indent(9) << "<valuemax>1</valuemax>\n"
                       << Indent(8) << "</parameter>\n"
                       << Indent(7) << "</effect>\n"
                       << Indent(6) << "</filter>\n";
            }
            // R9 -- the framing. One key is the parameters' values; more
            // are keys, in the clip's <in> basis.
            if (!clip.motion.empty()) {
                const bool keyed = clip.motion.size() > 1;
                const ResolveXmlMotionKey& first = clip.motion.front();
                const auto center = [&](const ResolveXmlMotionKey& key,
                                        size_t depth) {
                    output << Indent(depth) << "<value>\n"
                           << Indent(depth + 1) << "<horiz>"
                           << FormatResolveXmlFraction(key.center_x)
                           << "</horiz>\n"
                           << Indent(depth + 1) << "<vert>"
                           << FormatResolveXmlFraction(key.center_y)
                           << "</vert>\n"
                           << Indent(depth) << "</value>\n";
                };
                output << Indent(6) << "<filter>\n"
                       << Indent(7) << "<enabled>TRUE</enabled>\n"
                       << Indent(7) << "<start>-1</start>\n"
                       << Indent(7) << "<end>-1</end>\n"
                       << Indent(7) << "<effect>\n"
                       << Indent(8) << "<name>Basic Motion</name>\n"
                       << Indent(8) << "<effectid>basic</effectid>\n"
                       << Indent(8) << "<effecttype>motion</effecttype>\n"
                       << Indent(8) << "<mediatype>video</mediatype>\n"
                       << Indent(8)
                       << "<effectcategory>motion</effectcategory>\n"
                       << Indent(8) << "<parameter>\n"
                       << Indent(9) << "<name>Scale</name>\n"
                       << Indent(9) << "<parameterid>scale</parameterid>\n"
                       << Indent(9) << "<valuemin>0</valuemin>\n"
                       << Indent(9) << "<valuemax>10000</valuemax>\n"
                       << Indent(9) << "<value>"
                       << FormatResolveXmlFraction(first.scale_percent)
                       << "</value>\n";
                if (keyed) {
                    for (const ResolveXmlMotionKey& key : clip.motion) {
                        output << Indent(9) << "<keyframe>\n"
                               << Indent(10) << "<when>" << key.when
                               << "</when>\n"
                               << Indent(10) << "<value>"
                               << FormatResolveXmlFraction(key.scale_percent)
                               << "</value>\n"
                               << Indent(9) << "</keyframe>\n";
                    }
                }
                output << Indent(8) << "</parameter>\n"
                       << Indent(8) << "<parameter>\n"
                       << Indent(9) << "<name>Center</name>\n"
                       << Indent(9) << "<parameterid>center</parameterid>\n";
                center(first, 9);
                if (keyed) {
                    for (const ResolveXmlMotionKey& key : clip.motion) {
                        output << Indent(9) << "<keyframe>\n"
                               << Indent(10) << "<when>" << key.when
                               << "</when>\n";
                        center(key, 10);
                        output << Indent(9) << "</keyframe>\n";
                    }
                }
                output << Indent(8) << "</parameter>\n"
                       << Indent(7) << "</effect>\n"
                       << Indent(6) << "</filter>\n";
            }
            output << Indent(5) << "</clipitem>\n";
        }
        for (const ResolveXmlTransition& transition : timeline.transitions) {
            if (transition.video_layer != layer) continue;
            output << Indent(5) << "<transitionitem>\n";
            WriteRate(output, sequenceTimebase, 6);
            output << Indent(6) << "<start>" << transition.start << "</start>\n"
                   << Indent(6) << "<end>" << transition.end << "</end>\n"
                   << Indent(6) << "<alignment>" << transition.alignment
                   << "</alignment>\n"
                   << Indent(6) << "<effect>\n"
                   << Indent(7) << "<name>Cross Dissolve</name>\n"
                   << Indent(7) << "<effectid>Cross Dissolve</effectid>\n"
                   << Indent(7) << "<effecttype>transition</effecttype>\n"
                   << Indent(7) << "<mediatype>video</mediatype>\n"
                   << Indent(7) << "<effectcategory>Dissolve</effectcategory>\n"
                   << Indent(7) << "<startratio>0</startratio>\n"
                   << Indent(7) << "<endratio>1</endratio>\n"
                   << Indent(7) << "<reverse>FALSE</reverse>\n"
                   << Indent(6) << "</effect>\n"
                   << Indent(5) << "</transitionitem>\n";
        }
        output << Indent(5) << "<enabled>TRUE</enabled>\n"
               << Indent(5) << "<locked>FALSE</locked>\n"
               << Indent(4) << "</track>\n";
    }
    output << Indent(4) << "<format>\n"
           << Indent(5) << "<samplecharacteristics>\n"
           << Indent(6) << "<width>" << timeline.width << "</width>\n"
           << Indent(6) << "<height>" << timeline.height << "</height>\n"
           << Indent(6) << "<pixelaspectratio>square</pixelaspectratio>\n";
    WriteRate(output, sequenceTimebase, 6);
    output << Indent(5) << "</samplecharacteristics>\n"
           << Indent(4) << "</format>\n"
           << Indent(3) << "</video>\n"
           << Indent(2) << "</media>\n"
           << Indent(1) << "</sequence>\n"
           << "</xmeml>\n";
    return output.str();
}

bool PublishNewInterchangeFile(const std::string& path,
                               const std::string& bytes, std::string& errorName,
                               std::string& error) {
    return WriteNewXml(path, bytes, errorName, error);
}

bool BuildAndWriteResolveXml(Document document,
                             const std::string& packageDirectory,
                             const std::string& xmlPath,
                             std::string& reportJson, std::string& errorName,
                             std::string& error, ResolveScaling scaling) {
    // Media paths are stored relative to the package; the bridge matches them
    // against Resolve's Media Pool, which knows only absolute paths.
    const std::filesystem::path base(packageDirectory);
    for (LibraryMedia& media : document.library) {
        std::filesystem::path stored(media.path);
        if (stored.is_relative()) stored = base / stored;
        std::error_code pathError;
        const std::filesystem::path resolved =
            std::filesystem::weakly_canonical(stored, pathError);
        if (!pathError) media.path = resolved.string();
    }
    ResolveXmlTimeline timeline;
    if (!BuildResolveXmlTimeline(document, timeline, error, scaling)) {
        errorName = "InvalidExport";
        return false;
    }
    const std::string xml = WriteResolveXml(timeline);
    if (!WriteNewXml(xmlPath, xml, errorName, error)) {
        return false;
    }

    int32_t retimed = 0;
    int32_t framed = 0;
    for (const ResolveXmlClip& clip : timeline.clips) {
        if (!IsUnitSpeed(clip.speed)) ++retimed;
        if (!clip.motion.empty()) ++framed;
    }
    std::ostringstream report;
    report << "{\"ok\":true,\"schema\":\"cutmachine.resolve-xml.v1\""
           << ",\"path\":\"" << mcp_json::EscapeJsonString(xmlPath)
           << "\",\"name\":\"" << mcp_json::EscapeJsonString(timeline.name)
           << "\",\"frame_rate\":{\"num\":" << timeline.frame_rate.num
           << ",\"den\":" << timeline.frame_rate.den
           << "},\"width\":" << timeline.width
           << ",\"height\":" << timeline.height
           << ",\"duration_frames\":" << timeline.duration
           << ",\"clips\":" << timeline.clips.size()
           << ",\"retimed_clips\":" << retimed
           << ",\"transitions\":" << timeline.transitions.size()
           << ",\"video_layers\":" << timeline.video_layers
           << ",\"media\":" << timeline.media.size()
           << ",\"audio_tracks_not_sent\":" << timeline.audio_tracks_not_sent
           << ",\"multicam_angles_not_sent\":"
           << timeline.multicam_angles_not_sent
           << ",\"framed_clips\":" << framed << ",\"resolve_scaling\":\""
           << (scaling == ResolveScaling::Fit    ? "fit"
               : scaling == ResolveScaling::Fill ? "fill"
                                                 : "undeclared")
           << '"' << ",\"grades_not_sent\":" << timeline.grades_not_sent
           << ",\"blurs_not_sent\":" << timeline.blurs_not_sent
           << ",\"masks_not_sent\":" << timeline.masks_not_sent
           << ",\"start_timecodes\":\"placeholder\"}\n";
    reportJson = report.str();
    errorName.clear();
    error.clear();
    return true;
}

int ExportResolveXmlCommand(const std::string& projectPath,
                            const std::string& xmlPath, std::string& output,
                            const std::string& timelineId,
                            ResolveScaling scaling) {
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
    std::string errorName;
    if (!BuildAndWriteResolveXml(
            project.MakeDocument(selectedTimeline),
            std::filesystem::absolute(projectPath).parent_path().string(),
            xmlPath, output, errorName, error, scaling)) {
        return FailCliCommand(errorName, error, output);
    }
    return 0;
}
