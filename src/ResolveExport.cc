#include "ResolveExport.h"

#include "Cli.h"
#include "ClipFraming.h"
#include "ClipSpeed.h"
#include "Json.h"
#include "Multicam.h"
#include "ProjectStorage.h"

#include <algorithm>
#include <filesystem>
#include <set>
#include <sstream>

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
                                std::string& error) {
    ResolveTimelineExport built;
    built.name = document.sequence.name;
    built.frame_rate = document.sequence.frame_rate;
    built.width = document.sequence.width;
    built.height = document.sequence.height;

    // Out of lot -- the author, 29 October 2026. AppendToTimeline appends clips
    // that play: a disabled one (ClipEnabled.h) would come back into the
    // film in Resolve, seen and heard, under a success. Refused by name;
    // the FCP7 XML route carries it disabled.
    for (const DocumentTrack& track : document.sequence.tracks) {
        for (const DocumentClip& clip : track.clips) {
            if (!clip.enabled) {
                error = "clip '" + clip.id +
                        "' is disabled, which AppendToTimeline does not "
                        "carry: enable or remove it, or send this montage "
                        "as a FCP7 XML instead (--export-resolve-xml)";
                return false;
            }
        }
    }

    // Video layers are numbered from the bottom track up, by DocumentTrack
    // index, so layer 0 is the base cut and every higher layer covers it.
    std::vector<const DocumentTrack*> videoTracks;
    for (const DocumentTrack& track : document.sequence.tracks)
        if (track.kind == "video") videoTracks.push_back(&track);
    std::stable_sort(videoTracks.begin(), videoTracks.end(),
                     [](const DocumentTrack* left, const DocumentTrack* right) {
                         return left->index < right->index;
                     });

    // Base layer first, then each overlay in turn, and within a layer in
    // timeline order. Append order no longer decides where a clip lands --
    // record_frame does -- but sending a track at a time keeps the report
    // readable and matches the order a viewer builds the picture in.
    struct Ordered {
        int32_t layer;
        RationalTime timeline_in;
        const DocumentClip* clip;
    };
    // A clip keeps its sound only when the montage plays exactly that
    // range on an audio track. Anything else -- an overlay laid over
    // someone else's words, or a picture the editor deliberately
    // silenced -- goes over picture-only, because Resolve would otherwise
    // bring its room tone along and lay it under the interview.
    const bool anySolo = std::any_of(
        document.sequence.tracks.begin(), document.sequence.tracks.end(),
        [](const DocumentTrack& track) {
            return track.kind == "audio" && track.solo;
        });
    const auto playsOwnSound = [&](const DocumentClip& clip, bool audibleOnly) {
        for (const DocumentTrack& track : document.sequence.tracks) {
            if (track.kind != "audio" ||
                (audibleOnly && (track.muted || (anySolo && !track.solo)))) {
                continue;
            }
            for (const DocumentClip& sound : track.clips) {
                if (sound.source_id == clip.source_id &&
                    sound.timeline_in == clip.timeline_in &&
                    sound.source_in == clip.source_in &&
                    sound.duration == clip.duration) {
                    return true;
                }
            }
        }
        return false;
    };
    // F1.5 -- ROADMAP.md. An inactive multicam angle is not in the picture
    // (Multicam.h), so it is not sent: Resolve would stack it like any clip.
    // When its own sound still plays in CUTMACHINE, though, this trip cannot
    // carry that sound without its picture, so the angle is refused rather
    // than sent silent or visible. Muting that audio track lifts the refusal.
    const std::set<Ulid> multicamHidden =
        MulticamHiddenClipIds(document.sequence);
    std::vector<Ordered> ordered;
    for (size_t layer = 0; layer < videoTracks.size(); ++layer) {
        for (const DocumentClip& clip : videoTracks[layer]->clips) {
            if (multicamHidden.count(clip.id) != 0) {
                if (playsOwnSound(clip, true)) {
                    error = "clip '" + clip.id +
                            "' is an inactive multicam angle whose own "
                            "sound plays on an audio track; the Resolve "
                            "return trip can only send that sound with its "
                            "picture (mute or remove that sound first)";
                    return false;
                }
                continue;
            }
            // MOTION-2026-09 M4 -- a motion clip would travel as a media
            // with alpha posed on its track: not done, and not promised
            // until it has been checked in Resolve.
            if (IsMotionClip(clip)) {
                error = "clip '" + clip.id +
                        "' shows a motion composition; the Resolve return "
                        "trip does not carry motion clips yet (as a video "
                        "with alpha on its track, to validate in Resolve "
                        "first): remove it, or export the timeline instead";
                return false;
            }
            ordered.push_back(
                {static_cast<int32_t>(layer), clip.timeline_in, &clip});
        }
    }
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const Ordered& left, const Ordered& right) {
                         if (left.layer != right.layer)
                             return left.layer < right.layer;
                         return left.timeline_in < right.timeline_in;
                     });
    if (ordered.empty()) {
        error = "the active timeline has no video clip to send";
        return false;
    }
    // R9 -- docs/roadmap/REEL-2026-09.md. A dissolve used to be dropped here
    // without a word: AppendToTimeline lays clips end to end and knows no
    // transition, so the montage that arrived in Resolve was not the montage
    // the document holds. Now that the XML route carries one, the loss
    // becomes a named refusal rather than a montage that is quietly not the
    // one that was sent.
    if (!document.sequence.transitions.empty()) {
        error = "the timeline carries " +
                std::to_string(document.sequence.transitions.size()) +
                " transition(s); AppendToTimeline lays clips end to end and "
                "would drop every one of them, so send this montage as a "
                "FCP7 XML instead (--export-resolve-xml, then "
                "resolve_bridge.py --send-xml)";
        return false;
    }

    // R6 -- docs/roadmap/REEL-2026-09.md. Nor does this route carry a
    // sequence look: the timeline's node graph has no node and the API
    // creates none, so a LUT would have to go through a colour group's
    // post-clip graph, which R9 has not written for either route. Named
    // here rather than dropped, like the transitions above.
    if (!document.sequence.look_lut.digest.empty()) {
        error = "the timeline carries a sequence look LUT ('" +
                document.sequence.look_lut.name +
                "'), which no return route carries yet: R9 sends a look "
                "through a colour group's post-clip node";
        return false;
    }

    for (const Ordered& item : ordered) {
        const DocumentClip& clip = *item.clip;
        const auto media =
            std::find_if(document.library.begin(), document.library.end(),
                         [&](const LibraryMedia& entry) {
                             return entry.id == clip.source_id;
                         });
        if (media == document.library.end()) {
            error = "clip '" + clip.id + "' references media '" +
                    clip.source_id + "' that is not in the library";
            return false;
        }
        if (media->rate.num <= 0 || media->rate.den <= 0) {
            error = "media '" + media->filename + "' has no usable frame rate";
            return false;
        }
        // R3 -- docs/roadmap/REEL-2026-09.md. AppendToTimeline carries no
        // speed, and no key of TimelineItem.SetProperty sets one (measured
        // on Resolve 20.3, 19 September). Sending a slowed shot as a plain
        // source range would put the right frames of the wrong length on
        // the timeline, under a success. R9 has since built the route that
        // does carry one, so this refusal names it instead of ending here.
        if (ClipHasSpeed(clip)) {
            error = ClipSpeedRefusal(clip, "the Resolve timeline export") +
                    "; AppendToTimeline carries no speed, so send this "
                    "montage as a FCP7 XML instead "
                    "(--export-resolve-xml, then resolve_bridge.py "
                    "--send-xml)";
            return false;
        }
        // R1, R2 -- docs/roadmap/REEL-2026-09.md. AppendToTimeline knows
        // nothing of a framing or of a key; the FCP7 XML route carries both
        // since 24 September (Basic Motion, ResolveXmlExport.h). A framed or
        // animated clip sent this way would come back unframed, or frozen,
        // under a success: refused by name, pointing at the route that
        // carries it.
        if (ClipHasFraming(clip)) {
            error = "clip '" + clip.id + "' on '" + media->filename +
                    "' carries a framing, which this route does not send; "
                    "send the timeline as an FCP7 XML instead "
                    "(export_resolve_xml, then resolve_bridge.py --send-xml)";
            return false;
        }
        if (ClipHasAnimation(clip)) {
            error = "clip '" + clip.id + "' on '" + media->filename +
                    "' animates a parameter, which this route does not "
                    "send; send the timeline as an FCP7 XML instead "
                    "(export_resolve_xml, then resolve_bridge.py --send-xml)";
            return false;
        }
        // R9 -- nor an opacity, which no route to Resolve carries: a
        // translucent layer would arrive opaque, under a success.
        if (clip.opacity.num != clip.opacity.den) {
            error = "clip '" + clip.id + "' on '" + media->filename +
                    "' is not fully opaque, which no route to Resolve "
                    "carries: set its opacity in Resolve instead";
            return false;
        }
        // EF3 -- docs/roadmap/EFFETS-2026-10.md. Nor a blur, which
        // AppendToTimeline has no field for: the shot would arrive sharp,
        // under a success.
        if (ClipHasBlur(clip)) {
            error = "clip '" + clip.id + "' on '" + media->filename +
                    "' carries a blur, which this route does not send: "
                    "remove it (set_clip_blur to 0) and blur the shot in "
                    "Resolve, or send the timeline as an FCP7 XML, which "
                    "counts it under blurs_not_sent";
            return false;
        }
        // PA8 -- docs/roadmap/PARITE-2026-11.md. Nor a mask: the shot would
        // arrive whole, over what the mask let show, under a success.
        if (ClipHasMask(clip)) {
            error = "clip '" + clip.id + "' on '" + media->filename +
                    "' carries a mask, which this route does not send: "
                    "remove it (set_clip_mask with shape 'none') and mask "
                    "the shot in Resolve, or send the timeline as an FCP7 "
                    "XML, which counts it under masks_not_sent";
            return false;
        }
        int64_t startFrame = 0;
        int64_t durationFrames = 0;
        if (!ExactFrameCount(clip.source_in, media->rate, startFrame) ||
            !ExactFrameCount(clip.duration, media->rate, durationFrames)) {
            error = "clip '" + clip.id + "' on '" + media->filename +
                    "' does not start and end on whole source frames at " +
                    std::to_string(media->rate.num) + "/" +
                    std::to_string(media->rate.den) +
                    "; sending it would move the cut";
            return false;
        }
        if (durationFrames <= 0) {
            error = "clip '" + clip.id + "' is shorter than one source frame";
            return false;
        }
        int64_t recordFrame = 0;
        if (!ExactFrameCount(clip.timeline_in, document.sequence.frame_rate,
                             recordFrame)) {
            error = "clip '" + clip.id +
                    "' does not sit on a whole sequence "
                    "frame; sending it would move the cut";
            return false;
        }
        const bool withAudio = playsOwnSound(clip, false);

        ResolveTimelineClip sent;
        sent.filename = media->filename;
        sent.path = media->path;
        sent.video_layer = item.layer;
        sent.record_frame = recordFrame;
        sent.with_audio = withAudio;
        sent.start_frame = startFrame;
        // Resolve's clipInfo endFrame is exclusive: AppendToTimeline builds
        // end-start frames. Verified by sending a 9-clip cut and counting what
        // came back -- an inclusive end lost exactly one frame per clip.
        sent.end_frame = startFrame + durationFrames;
        built.clips.push_back(std::move(sent));
    }
    exported = std::move(built);
    error.clear();
    return true;
}

std::string SerializeResolveTimelineExport(
    const ResolveTimelineExport& exported) {
    std::ostringstream output;
    output << "{\"ok\":true,\"schema\":\"cutmachine.resolve-timeline.v2\""
           << ",\"name\":\"" << mcp_json::EscapeJsonString(exported.name)
           << "\",\"frame_rate\":{\"num\":" << exported.frame_rate.num
           << ",\"den\":" << exported.frame_rate.den
           << "},\"width\":" << exported.width
           << ",\"height\":" << exported.height << ",\"clips\":[";
    for (size_t index = 0; index < exported.clips.size(); ++index) {
        const ResolveTimelineClip& clip = exported.clips[index];
        if (index) output << ',';
        output << "{\"path\":\"" << mcp_json::EscapeJsonString(clip.path)
               << "\",\"filename\":\""
               << mcp_json::EscapeJsonString(clip.filename)
               << "\",\"start_frame\":" << clip.start_frame
               << ",\"end_frame\":" << clip.end_frame
               << ",\"video_layer\":" << clip.video_layer
               << ",\"record_frame\":" << clip.record_frame
               << ",\"with_audio\":" << (clip.with_audio ? "true" : "false")
               << "}";
    }
    output << "]}\n";
    return output.str();
}

int ExportResolveTimelineCommand(const std::string& projectPath,
                                 std::string& output,
                                 const std::string& timelineId) {
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
    const std::filesystem::path base =
        std::filesystem::absolute(projectPath).parent_path();
    for (LibraryMedia& media : document.library) {
        std::filesystem::path stored(media.path);
        if (stored.is_relative()) stored = base / stored;
        std::error_code pathError;
        const std::filesystem::path resolved =
            std::filesystem::weakly_canonical(stored, pathError);
        if (!pathError) media.path = resolved.string();
    }
    ResolveTimelineExport exported;
    if (!BuildResolveTimelineExport(document, exported, error)) {
        return FailCliCommand("InvalidExport", error, output);
    }
    output = SerializeResolveTimelineExport(exported);
    return 0;
}
