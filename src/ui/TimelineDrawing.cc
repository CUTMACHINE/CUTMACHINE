#include "TimelineDrawing.h"

#include "ClipBadges.h"
#include "MotionClip.h"
#include "Timecode.h"
#include "TransitionRender.h"

#include <algorithm>
#include <cmath>
#include <filesystem>

std::array<float, 3> ClipColor(const Ulid& sourceId, bool audio) {
    uint64_t hash = UINT64_C(1469598103934665603);
    for (const unsigned char byte : sourceId) {
        hash ^= byte;
        hash *= UINT64_C(1099511628211);
    }
    const float variation = static_cast<float>(hash & 0xff) / 255.0f;
    const ui::theme::Color base =
        audio ? ui::theme::kTrackAudio : ui::theme::kTrackVideo;
    const float lift = (variation - 0.5f) * 0.06f;
    return {std::clamp(base.r + lift, 0.0f, 1.0f),
            std::clamp(base.g + lift, 0.0f, 1.0f),
            std::clamp(base.b + lift, 0.0f, 1.0f)};
}

std::string RushDisplayName(const Project& project, const LibraryMedia& media) {
    if (const ProjectBinMetadata* metadata = project.FindBinMetadata(media.id);
        metadata && !metadata->display_name.empty())
        return metadata->display_name;
    return media.filename;
}

std::string TimelineClipName(const Document& document, const Project& project,
                             const Ulid& sourceId) {
    std::string name;
    if (const LibraryMedia* media = document.FindLibraryMedia(sourceId))
        name = RushDisplayName(project, *media);
    if (name.empty()) {
        if (const DocumentSource* source = document.FindSource(sourceId))
            name = std::filesystem::path(source->path).filename().string();
    }
    const size_t dot = name.find_last_of('.');
    if (dot != std::string::npos && dot > 0) name.resize(dot);
    return name;
}

std::string TimelineDrawnClipName(const Document& document,
                                  const Project& project,
                                  const DocumentClip* clip,
                                  const Ulid& sourceId) {
    const DocumentComposition* composition =
        clip && IsMotionClip(*clip)
            ? document.FindComposition(clip->composition_id)
            : nullptr;
    if (composition != nullptr) return composition->name;
    if (clip && sourceId.empty()) return clip->caption_text;
    return TimelineClipName(document, project, sourceId);
}

ui::theme::Color MarkerDisplayColor(const DocumentMarker& marker) {
    TransitionColor parsed;
    std::string error;
    if (!ParseTransitionColor(marker.color, parsed, error))
        return ui::theme::kMarker;
    return {static_cast<float>(parsed.red) / 255.0f,
            static_cast<float>(parsed.green) / 255.0f,
            static_cast<float>(parsed.blue) / 255.0f, 1.0f};
}

std::string SyncDriftLabel(const RationalTime& drift, MediaRate frameRate) {
    const int64_t frames = drift.to_frames(frameRate.num, frameRate.den);
    const RationalTime frameQuantized{
        frames * static_cast<int64_t>(frameRate.den), frameRate.num};
    int64_t value = frames;
    const char* unit = "f";
    if (frameQuantized != drift) {
        value = drift.to_frames(48000);
        unit = "smp";
    }
    return std::string(value > 0 ? "+" : "") + std::to_string(value) + unit;
}

TimelineRenderData BuildTimelineRenderData(const TimelineDrawingInput& in) {
    TimelineRenderData data;
    data.color_management = in.document->color_management;
    data.display_sdr_preview = true;
    data.sequence_width = in.document->sequence.width;
    data.sequence_height = in.document->sequence.height;
    const double width = in.width;
    const double timelineHeight = in.timeline_height;
    data.video_height = in.video_height;
    const double top = data.video_height;
    auto add = [&](double x, double y, double w, double h, float r, float g,
                   float b, float a = 1.0f) {
        if (w > 0.0 && h > 0.0)
            data.overlays.emplace_back(MetalRect{x, y, w, h, r, g, b, a});
    };
    const auto addColor = [&](double x, double y, double w, double h,
                              ui::theme::Color color) {
        add(x, y, w, h, color.r, color.g, color.b, color.a);
    };
    const auto addV = [&](double x, double y, double w, double h,
                          ui::theme::Color topColor,
                          ui::theme::Color bottomColor) {
        if (w > 0.0 && h > 0.0)
            data.overlays.emplace_back(
                MetalRect{x, y, w, h, topColor, bottomColor});
    };
    const auto addText =
        [&](double x, double y, double maxWidth, double pointSize,
            const std::string& text, ui::theme::Color color,
            MetalFontFace face = MetalFontFace::Mono, bool bold = false) {
            if (!text.empty() && maxWidth > 0.0)
                data.overlays.emplace_back(MetalText{x, y, maxWidth, pointSize,
                                                     text, color, face, bold});
        };
    const auto addIcon = [&](double x, double y, double size,
                             const std::string& name, ui::theme::Color color) {
        if (size > 0.0 && !name.empty())
            data.overlays.emplace_back(
                MetalIcon{x, y, size, size, name, color});
    };

    const auto topmost =
        std::find_if(in.requested->rbegin(), in.requested->rend(),
                     [](const ResolvedSlot& slot) { return slot.active; });
    if (topmost != in.requested->rend() &&
        in.offline_source_ids->count(topmost->sourceId) != 0)
        add(0.0, 0.0, width, data.video_height, 0.025f, 0.025f, 0.025f);

    addColor(0.0, top, width, timelineHeight, ui::theme::kSurfaceBase);
    addV(0.0, top, width, kTimelineToolbarHeight, ui::theme::kSurfaceControl,
         ui::theme::kSurfaceRaised);
    addColor(0.0, top + kTimelineToolbarHeight, width, kTimelineScaleHeight,
             ui::theme::kSurfaceRaised);
    addColor(0.0, top + kTimelineToolbarHeight + kTimelineScaleHeight, width,
             kTimelineRenderBandHeight, ui::theme::kSurfaceSunken);
    addColor(0.0, top + kTimelineRulerHeight - 1.0, width, 1.0,
             ui::theme::kSeparator);

    constexpr double toolStart = 4.0;
    constexpr double toolWidth = 24.0;
    const std::array<const char*, 5> toolIcons{{
        "mouse-pointer-2",
        "hand",
        "search",
        "scissors",
        "unfold-horizontal",
    }};
    for (int index = 0; index < 5; ++index) {
        const bool active = static_cast<int>(in.effective_tool) == index;
        const bool hovered = in.hovered_timeline_tool == index;
        const double x = toolStart + index * toolWidth;
        addV(x, top + 2.0, toolWidth - 2.0, 22.0,
             active || hovered ? ui::theme::kSurfaceControlHi
                               : ui::theme::kSurfaceControl,
             active ? ui::theme::kSurfaceControl : ui::theme::kSurfaceRaised);
        addColor(x, top + 2.0, toolWidth - 2.0, 1.0, ui::theme::kEdgeLight);
        if (active)
            addColor(x, top + 22.0, toolWidth - 2.0, 2.0, ui::theme::kAccent);
        if (hovered)
            addColor(x, top + 2.0, toolWidth - 2.0, 2.0,
                     ui::theme::kTextPrimary);
        addIcon(x + 5.0, top + 6.0, 12.0, toolIcons[index],
                active ? ui::theme::kTextPrimary : ui::theme::kTextTertiary);
    }
    const bool snapping = in.interaction->SnappingEnabled();
    addV(128.0, top + 2.0, 38.0, 22.0,
         snapping ? ui::theme::kSurfaceControlHi : ui::theme::kSurfaceControl,
         ui::theme::kSurfaceRaised);
    addColor(128.0, top + 2.0, 38.0, 1.0, ui::theme::kEdgeLight);
    if (snapping) addColor(128.0, top + 22.0, 38.0, 2.0, ui::theme::kAccent);
    addIcon(134.0, top + 7.0, 11.0, "magnet",
            snapping ? ui::theme::kAccent : ui::theme::kTextTertiary);
    addText(148.0, top + 6.0, 20.0, ui::theme::kFontSizeCaption, "8",
            ui::theme::kTextSecondary, MetalFontFace::Mono, true);
    addV(170.0, top + 2.0, 44.0, 22.0,
         in.linked_selection ? ui::theme::kSurfaceControlHi
                             : ui::theme::kSurfaceControl,
         ui::theme::kSurfaceRaised);
    addColor(170.0, top + 2.0, 44.0, 1.0, ui::theme::kEdgeLight);
    if (in.linked_selection)
        addColor(170.0, top + 22.0, 44.0, 2.0, ui::theme::kAccent);
    const ui::theme::Color linkColor = in.linked_selection
                                           ? ui::theme::kTextPrimary
                                           : ui::theme::kTextTertiary;
    addIcon(174.0, top + 7.0, 11.0, "link", linkColor);
    addText(188.0, top + 6.0, 28.0, ui::theme::kFontSizeCaption, "A/V",
            linkColor, MetalFontFace::Mono, true);
    const std::array<const char*, 3> transportLabels{{"J", "K", "L"}};
    for (int index = 0; index < 3; ++index) {
        const bool active = index == 0   ? in.playback_direction < 0
                            : index == 1 ? in.playback_direction == 0
                                         : in.playback_direction > 0;
        const double x = 222.0 + index * 24.0;
        addV(x, top + 2.0, 22.0, 22.0,
             active ? ui::theme::kSurfaceControlHi : ui::theme::kSurfaceControl,
             ui::theme::kSurfaceRaised);
        addColor(x, top + 2.0, 22.0, 1.0, ui::theme::kEdgeLight);
        if (active) addColor(x, top + 22.0, 22.0, 2.0, ui::theme::kAccent);
        addText(x + 7.0, top + 6.0, 8.0, ui::theme::kFontSizeCaption,
                transportLabels[index],
                active ? ui::theme::kTextPrimary : ui::theme::kTextSecondary,
                MetalFontFace::Mono, true);
    }
    addText(std::max(300.0, width - 102.0), top + 6.0, 96.0,
            ui::theme::kFontSizeCaption,
            FormatTimecode(in.requested_position, in.playhead_rate),
            ui::theme::kTextSecondary, MetalFontFace::Mono, true);
    if (in.has_valid_timeline_range) {
        const double rawIn = in.viewport->TimeToX(*in.timeline_in);
        const double rawOut = in.viewport->TimeToX(*in.timeline_out);
        const double left = std::max(in.viewport->header_width, rawIn);
        const double right = std::min(width, rawOut);
        if (right > left) {
            add(left, top + kTimelineToolbarHeight, right - left,
                kTimelineScaleHeight + kTimelineRenderBandHeight,
                ui::theme::kAccent.r, ui::theme::kAccent.g,
                ui::theme::kAccent.b, 0.18f);
        }
        if (rawIn >= in.viewport->header_width && rawIn <= width)
            addColor(rawIn, top + kTimelineToolbarHeight, 1.0,
                     timelineHeight - kTimelineToolbarHeight,
                     ui::theme::kMarkIn);
        if (rawOut >= in.viewport->header_width && rawOut <= width)
            addColor(rawOut - 1.0, top + kTimelineToolbarHeight, 1.0,
                     timelineHeight - kTimelineToolbarHeight,
                     ui::theme::kMarkOut);
    } else {
        if (in.timeline_in) {
            const double x = in.viewport->TimeToX(*in.timeline_in);
            if (x >= in.viewport->header_width && x <= width)
                addColor(x, top + kTimelineToolbarHeight, 1.0,
                         timelineHeight - kTimelineToolbarHeight,
                         ui::theme::kMarkIn);
        }
        if (in.timeline_out) {
            const double x = in.viewport->TimeToX(*in.timeline_out);
            if (x >= in.viewport->header_width && x <= width)
                addColor(x - 1.0, top + kTimelineToolbarHeight, 1.0,
                         timelineHeight - kTimelineToolbarHeight,
                         ui::theme::kMarkOut);
        }
    }
    const auto tracks = TimelineTracksInDisplayOrder((*in.document));
    bool sawVideoTrack = false;
    bool drewAudioDivider = false;
    for (size_t index = 0; index < tracks.size(); ++index) {
        const double y =
            top + kTimelineRulerHeight + index * in.viewport->track_height;
        if (y >= top + timelineHeight) break;
        const bool video = tracks[index]->kind == "video";
        const bool caption = tracks[index]->kind == "caption";
        if (video) sawVideoTrack = true;
        if (!video && sawVideoTrack && !drewAudioDivider) {
            addColor(0.0, y - 2.0, width, 2.0, ui::theme::kBorderStrong);
            drewAudioDivider = true;
        }
        addColor(0.0, y, width, in.viewport->track_height,
                 ui::theme::kSurfaceLane);
        addV(0.0, y, in.viewport->header_width, in.viewport->track_height,
             ui::theme::kSurfaceRaised, ui::theme::kSurfacePanel);
        addColor(0.0, y, 4.0, in.viewport->track_height,
                 ui::theme::TrackTint(video));
        addText(8.0, y + 4.0, 28.0, ui::theme::kFontSizeSmall,
                TrackDisplayName((*in.document), tracks[index]->id),
                ui::theme::kTextPrimary, MetalFontFace::Mono, true);
        addText(8.0, y + 20.0, 30.0, ui::theme::kFontSizeCaption,
                video     ? "VID."
                : caption ? "SUB."
                          : "AUD.",
                ui::theme::kTextTertiary, MetalFontFace::Mono, true);

        const auto addPill = [&](double x, const std::string& label,
                                 bool active, int control,
                                 bool disabled = false) {
            const bool hovered = in.hovered_track_id == tracks[index]->id &&
                                 in.hovered_track_control == control;
            addV(x, y + 15.0, 16.0, 14.0,
                 disabled            ? ui::theme::kSurfacePanel
                 : active || hovered ? ui::theme::kSurfaceControlHi
                                     : ui::theme::kSurfaceControl,
                 ui::theme::kSurfaceRaised);
            if (!disabled)
                addColor(x, y + 15.0, 16.0, 1.0, ui::theme::kEdgeLight);
            if (active) addColor(x, y + 27.0, 16.0, 2.0, ui::theme::kAccent);
            if (hovered && !disabled)
                addColor(x, y + 15.0, 16.0, 2.0, ui::theme::kTextPrimary);
            addText(x + 5.0, y + 16.0, 8.0, ui::theme::kFontSizeCaption, label,
                    disabled ? ui::theme::kTextTertiary
                    : active ? ui::theme::kTextPrimary
                             : ui::theme::kTextSecondary,
                    MetalFontFace::Mono, true);
        };
        const auto addIconPill = [&](double x, const std::string& icon,
                                     bool active, int control) {
            const bool hovered = in.hovered_track_id == tracks[index]->id &&
                                 in.hovered_track_control == control;
            addV(x, y + 15.0, 16.0, 14.0,
                 active || hovered ? ui::theme::kSurfaceControlHi
                                   : ui::theme::kSurfaceControl,
                 ui::theme::kSurfaceRaised);
            addColor(x, y + 15.0, 16.0, 1.0, ui::theme::kEdgeLight);
            if (active) addColor(x, y + 27.0, 16.0, 2.0, ui::theme::kAccent);
            if (hovered)
                addColor(x, y + 15.0, 16.0, 2.0, ui::theme::kTextPrimary);
            addIcon(
                x + 3.0, y + 17.0, 10.0, icon,
                active ? ui::theme::kTextPrimary : ui::theme::kTextTertiary);
        };
        if (video || caption) {
            addIconPill(40.0, "eye", tracks[index]->visible, 0);
            addIconPill(58.0, "lock", tracks[index]->locked, 1);
        } else {
            addPill(40.0, "S", tracks[index]->solo, 0);
            addPill(58.0, "M", tracks[index]->muted, 1);
            addIconPill(76.0, "lock", tracks[index]->locked, 2);
        }
        addColor(0.0, y + in.viewport->track_height - 1.0, width, 1.0,
                 ui::theme::kSeparator);
    }
    // Out of lot -- the author, 29 October 2026. No `+` row under the headers
    // any more: a drop creates the tracks it needs (TimelineDropTracksAt), and
    // the Timeline menu, the timeline's own menu and Cmd+Shift+T still add
    // one.
    addColor(in.viewport->header_width - 1.0, top + kTimelineToolbarHeight, 1.0,
             timelineHeight - kTimelineToolbarHeight, ui::theme::kBorderStrong);
    // Graduations far enough apart for their timecodes: the widest label is
    // the one at the timeline's end, measured in the face the atlas draws
    // it with (monospaced, bold, caption size), plus its 4-point inset and
    // an 8-point gap before the next one.
    const double rulerLabelWidth = in.ruler_label_width;
    const std::vector<double> tickXs = in.viewport->TickXs(
        width, std::max(kTimelineMinTickSpacing, rulerLabelWidth + 12.0));
    for (size_t tickIndex = 0; tickIndex < tickXs.size(); ++tickIndex) {
        const double tickX = tickXs[tickIndex];
        const double rulerTop = top + kTimelineToolbarHeight;
        addColor(tickX, rulerTop + kTimelineScaleHeight - 7.0, 1.0, 7.0,
                 ui::theme::kTextTertiary);
        add(tickX, top + kTimelineRulerHeight, 1.0,
            std::max(0.0, timelineHeight - kTimelineRulerHeight),
            ui::theme::kLaneGrid.r, ui::theme::kLaneGrid.g,
            ui::theme::kLaneGrid.b, 0.58f);
        const RationalTime tickTime =
            in.viewport->XToTime(tickX, in.document->sequence.frame_rate.num);
        addText(tickX + 4.0, rulerTop + 4.0, rulerLabelWidth,
                ui::theme::kFontSizeCaption,
                FormatTimecode(tickTime, in.playhead_rate),
                ui::theme::kTextTertiary, MetalFontFace::Mono, true);
        if (tickIndex + 1 < tickXs.size()) {
            const double interval = tickXs[tickIndex + 1] - tickX;
            for (int subdivision = 1; subdivision < 4; ++subdivision) {
                addColor(tickX + interval * subdivision / 4.0,
                         rulerTop + kTimelineScaleHeight - 4.0, 1.0, 4.0,
                         ui::theme::kBorderStrong);
            }
        }
    }

    if (const auto& gap = in.interaction->SelectedGap()) {
        const auto track = std::find_if(tracks.begin(), tracks.end(),
                                        [&](const DocumentTrack* value) {
                                            return value->id == gap->track_id;
                                        });
        if (track != tracks.end()) {
            const double rawLeft = in.viewport->TimeToX(gap->start);
            const double rawRight =
                in.viewport->TimeToX(gap->start.add(gap->duration));
            const double left = std::max(in.viewport->header_width,
                                         std::min(rawLeft, rawRight));
            const double right = std::min(width, std::max(rawLeft, rawRight));
            const double y = top + kTimelineRulerHeight +
                             std::distance(tracks.begin(), track) *
                                 in.viewport->track_height;
            if (right > left && y < top + timelineHeight) {
                const double height = in.viewport->track_height;
                addColor(left, y, right - left, height,
                         ui::theme::WithAlpha(ui::theme::kAccent, 0.18f));
                addColor(left, y, right - left, 2.0, ui::theme::kAccent);
                addColor(left, y + height - 2.0, right - left, 2.0,
                         ui::theme::kAccent);
                addColor(left, y, 2.0, height, ui::theme::kAccent);
                addColor(right - 2.0, y, 2.0, height, ui::theme::kAccent);
                for (double stripe = left + 8.0; stripe < right; stripe += 12.0)
                    addColor(stripe, y + 5.0, 1.0, std::max(0.0, height - 10.0),
                             ui::theme::WithAlpha(ui::theme::kAccentHi, 0.35f));
            }
        }
    }

    // FC4 -- while J or L runs, the clips as the dynamic trim has them so
    // far; nothing is in the document before K.
    const Document& shownDocument = (*in.shown_document);
    const auto clips = VisibleTimelineClips(
        shownDocument, (*in.viewport), width, in.interaction->SelectedClipIds(),
        in.interaction->TrimPreview(), in.interaction->MovePreview());
    // GitHub #330 -- each audio clip's fades, from the rectangles just
    // computed, so that a trim or a move carries them: the veil on the body,
    // under the clip's name and badges, and the handles over everything,
    // where a press finds them (VisibleTimelineAudioFades).
    std::optional<TimelineFadePreview> fadePreview;
    if (in.fade_drag && in.fade_drag->reached)
        fadePreview = TimelineFadePreview{in.fade_drag->reached->clip_id,
                                          in.fade_drag->reached->fade_in,
                                          in.fade_drag->reached->fade_out};
    const std::vector<TimelineAudioFadeRect> fades =
        VisibleTimelineAudioFades(shownDocument, (*in.viewport), width, clips,
                                  (*in.retime_clip_ids), fadePreview);
    std::map<Ulid, const TimelineAudioFadeRect*> fadeOfClip;
    for (const TimelineAudioFadeRect& fade : fades)
        fadeOfClip[fade.clip_id] = &fade;
    for (const TimelineClipRect& clip : clips) {
        double left = std::min(clip.x, clip.x + clip.width);
        double right = std::max(clip.x, clip.x + clip.width);
        const bool headVisible =
            left >= in.viewport->header_width && left <= width;
        left = std::max(left, in.viewport->header_width);
        right = std::min(right, width);
        if (right <= left) continue;
        const double y = top + clip.y;
        if (y >= top + timelineHeight) continue;
        // REM5 -- a motion clip reads as one: its own colour, not a shot's
        // (the timeline draws no picture of any clip; M4's usage gap).
        const DocumentClip* drawnClip = in.document->FindClip(clip.clip_id);
        // Out of lot -- the author, 29 October 2026: a disabled clip is greyed,
        // its waveform and its name with it (ClipEnabled.h, UiTheme.h).
        const bool disabledClip = drawnClip && !drawnClip->enabled;
        const auto color =
            disabledClip ? std::array<float, 3>{ui::theme::kClipDisabled.r,
                                                ui::theme::kClipDisabled.g,
                                                ui::theme::kClipDisabled.b}
            : drawnClip && IsMotionClip(*drawnClip)
                ? std::array<float, 3>{ui::theme::kTrackMotion.r,
                                       ui::theme::kTrackMotion.g,
                                       ui::theme::kTrackMotion.b}
                : ClipColor(clip.source_id, clip.audio);
        if (!clip.valid)
            addColor(left, y + 2.0, right - left, 40.0,
                     ui::theme::WithAlpha(ui::theme::kError, 0.92f));
        else {
            const float previewLift = clip.preview ? 0.08f : 0.0f;
            // The moving rectangle is the exact overwrite candidate, not a
            // decorative ghost. Keep it opaque so the incoming result remains
            // readable over the clip it is about to replace.
            const float movingAlpha = 1.0f;
            const ui::theme::Color clipTop{
                std::min(1.0f, color[0] + 0.08f + previewLift),
                std::min(1.0f, color[1] + 0.08f + previewLift),
                std::min(1.0f, color[2] + 0.08f + previewLift), movingAlpha};
            const ui::theme::Color clipBottom{
                std::max(0.0f, color[0] - 0.05f + previewLift),
                std::max(0.0f, color[1] - 0.05f + previewLift),
                std::max(0.0f, color[2] - 0.05f + previewLift), movingAlpha};
            addV(left, y + 2.0, right - left, 40.0, clipTop, clipBottom);
        }
        addColor(left, y + 2.0, right - left, 1.0, ui::theme::kEdgeLight);
        if (headVisible)
            addColor(left, y + 3.0, 3.0, 38.0,
                     ui::theme::WithAlpha(ui::theme::kTextClip, 0.32f));
        if (clip.audio && clip.width > 0.0) {
            // The source window comes with the rectangle (TimelineView.h):
            // the trim preview's while a trim is held, speed included.
            const auto waveform = in.waveforms->find(clip.source_id);
            if (waveform != in.waveforms->end() && clip.source_in.rate > 0 &&
                clip.source_span.rate > 0 && clip.source_span.value > 0) {
                const AudioWaveform& data = waveform->second;
                const long double sourceStart =
                    static_cast<long double>(clip.source_in.value) /
                    clip.source_in.rate;
                const long double sourceDuration =
                    static_cast<long double>(clip.source_span.value) /
                    clip.source_span.rate;
                const double center = y + clip.height * 0.53;
                const double availableHeight = std::max(2.0, clip.height - 9.0);
                for (double x = std::floor(left) + 1.0; x < right; x += 2.0) {
                    const long double firstFraction = std::clamp(
                        static_cast<long double>((x - clip.x) / clip.width),
                        0.0L, 1.0L);
                    const long double lastFraction = std::clamp(
                        static_cast<long double>(
                            (std::min(right, x + 2.0) - clip.x) / clip.width),
                        0.0L, 1.0L);
                    size_t first = static_cast<size_t>(std::max<long double>(
                        0.0L, (sourceStart + sourceDuration * firstFraction) *
                                  data.peaks_per_second));
                    size_t last = static_cast<size_t>(std::max<long double>(
                        0.0L, (sourceStart + sourceDuration * lastFraction) *
                                  data.peaks_per_second));
                    first = std::min(first, data.peaks.size());
                    last =
                        std::min(std::max(last, first + 1), data.peaks.size());
                    if (first >= last) continue;
                    float peak = 0.0f;
                    for (size_t index = first; index < last; ++index)
                        peak = std::max(peak, data.peaks[index]);
                    const double barHeight = std::max(
                        1.0, static_cast<double>(peak) * availableHeight);
                    if (disabledClip)
                        addColor(x, center - barHeight * 0.5,
                                 std::min(1.25, right - x), barHeight,
                                 ui::theme::kClipDisabledWaveform);
                    else
                        add(x, center - barHeight * 0.5,
                            std::min(1.25, right - x), barHeight, 0.64f, 0.94f,
                            0.79f, 0.82f);
                }
            }
        }
        // GitHub #330 -- the fades: each column veiled from the top as deep
        // as the ramp takes the sound down, its lower edge the ramp's line.
        // A ramp steeper than a point per column would leave that line as
        // dots: each column's tread is joined to the one before it by a
        // riser on its left edge, so that it reads as one staircase.
        if (const auto fade = fadeOfClip.find(clip.clip_id);
            fade != fadeOfClip.end()) {
            const double bodyY = top + fade->second->body_y;
            const double bodyHeight = fade->second->body_height;
            const double line = ui::theme::kTimelineFadeLineHeight;
            const auto lineY = [&](double veil) {
                return bodyY + std::min(veil, bodyHeight - line);
            };
            double previousRight = -1.0;
            double previousY = 0.0;
            for (const TimelineFadeStep& step : fade->second->steps) {
                const double veil = (1.0 - step.gain) * bodyHeight;
                addColor(step.x, bodyY, step.width, veil,
                         ui::theme::kTimelineFadeVeil);
                const double y = lineY(veil);
                addColor(step.x, y, step.width, line,
                         ui::theme::kTimelineFadeLine);
                if (std::abs(step.x - previousRight) < 0.5 &&
                    std::abs(y - previousY) > line)
                    addColor(step.x, std::min(y, previousY), line,
                             std::abs(y - previousY) + line,
                             ui::theme::kTimelineFadeLine);
                previousRight = step.x + step.width;
                previousY = y;
            }
        }
        const double clipSpan = right - left;
        add(left, y + ui::theme::kTimelineClipNameBandTop, clipSpan, 14.0, 0.0f,
            0.0f, 0.0f, 0.40f);
        if (clipSpan >= ui::theme::kTimelineClipNameMinWidth) {
            const DocumentClip* documentClip =
                in.document->FindClip(clip.clip_id);
            const std::string name = TimelineDrawnClipName(
                (*in.document), (*in.project), documentClip, clip.source_id);
            addText(left + 5.0, y + (ui::theme::kTimelineClipNameBandTop + 1.0),
                    clipSpan - 10.0, ui::theme::kFontSizeSmall, name,
                    disabledClip ? ui::theme::kClipDisabledText
                                 : ui::theme::kTextClip,
                    MetalFontFace::Mono, true);
        }
        add(left, y + 40.0, clipSpan, 2.0, 0.0f, 0.0f, 0.0f, 0.28f);
        // PRD24 -- how far the sync badge reaches along the badge row, from
        // the row's own inset: what the clip carries is written after it.
        double syncBadgeReach = 0.0;
        if (clip.audio && clip.sync_drift && clip.sync_drift->value != 0) {
            const std::string label =
                SyncDriftLabel(*clip.sync_drift, in.playhead_rate);
            // PRD25 -- one plate of the badge row (ClipBadges.h): its label
            // used to sit four points too low, over the waveform.
            const PlacedClipBadge plate =
                PlaceClipPlate(label, ui::theme::kTimelineClipBadgeInset);
            if (clipSpan >=
                plate.x + plate.width + ui::theme::kTimelineClipBadgeEndInset) {
                add(left + plate.x, y + plate.y, plate.width, plate.height,
                    0.66f, 0.12f, 0.07f, 0.94f);
                addText(left + plate.text_x, y + plate.text_y, plate.text_width,
                        ui::theme::kFontSizeCaption, label,
                        ui::theme::kTextClip, MetalFontFace::Mono, true);
                syncBadgeReach = plate.width;
            } else {
                add(left + 3.0, y + 7.0, 3.0, std::min(11.0, clip.height - 9.0),
                    0.94f, 0.18f, 0.08f);
                syncBadgeReach = 3.0 + 3.0 - ui::theme::kTimelineClipBadgeInset;
            }
        }
        // PRD24 -- ROADMAP.md. What the clip carries, said on its rectangle:
        // ClipBadges.h decides which badges, in which order and where; this
        // only draws them. A clip that carries nothing gets no command here,
        // so its display list is the one it had before this ticket.
        const bool signageShown = drawnClip != nullptr && in.clip_signage;
        if (signageShown) {
            // PRD25 -- the agent's mark, while its last call's clips are
            // marked (AgentJournal.h).
            const ClipSignage signage = ClipSignageFor(
                (*in.document), *drawnClip,
                in.agent_journal->MarkedClips().count(clip.clip_id) != 0);
            for (const PlacedClipBadge& badge :
                 PlaceClipBadges(signage.badges, clipSpan, syncBadgeReach)) {
                addColor(left + badge.x, y + badge.y, badge.width, badge.height,
                         ClipBadgeFillColor(badge.kind));
                addText(left + badge.text_x, y + badge.text_y, badge.text_width,
                        ui::theme::kFontSizeCaption, badge.label,
                        ClipBadgeTextColor(badge.kind), MetalFontFace::Mono,
                        true);
            }
            // F1.5 -- an angle its multicam group keeps off screen: the
            // whole clip under a veil, badges and name included, down to
            // the descenders the name hangs under the body. Its edges and
            // its selection are drawn after, over the veil.
            if (signage.hidden_angle)
                addColor(left, y + ui::theme::kTimelineClipBodyTop, clipSpan,
                         ui::theme::kTimelineClipBodyHeight +
                             ui::theme::kTimelineClipNameOverhang,
                         ui::theme::kClipHiddenAngleVeil);
        }
        // Every edit boundary remains readable when adjacent clips share the
        // same source. Selection remains the only clip-level emphasis.
        const double outlineWidth = std::min(1.0, right - left);
        addColor(left, y + 2.0, outlineWidth, 40.0, ui::theme::kSeparator);
        addColor(right - outlineWidth, y + 2.0, outlineWidth, 40.0,
                 ui::theme::kSeparator);
        if (clip.selected) {
            const ui::theme::Color selection =
                clip.valid ? ui::theme::kTextPrimary : ui::theme::kError;
            addColor(left, y + 2.0, clipSpan, 2.0, selection);
            addColor(left, y + 40.0, clipSpan, 2.0, selection);
            addColor(left, y + 2.0, std::min(2.0, clipSpan), 40.0, selection);
            addColor(std::max(left, right - 2.0), y + 2.0,
                     std::min(2.0, clipSpan), 40.0, selection);
        }
        if (clip.selected && in.timeline_has_focus && clipSpan > 8.0) {
            addColor(left + 4.0, y + 6.0, clipSpan - 8.0, 1.0,
                     ui::theme::kAccent);
            addColor(left + 4.0, y + 37.0, clipSpan - 8.0, 1.0,
                     ui::theme::kAccent);
            addColor(left + 4.0, y + 6.0, 1.0, 32.0, ui::theme::kAccent);
            addColor(right - 5.0, y + 6.0, 1.0, 32.0, ui::theme::kAccent);
        }
        if (clip.moving) {
            addColor(left, y + 2.0, clipSpan, 2.0, ui::theme::kAccent);
            addColor(left, y + 40.0, clipSpan, 2.0, ui::theme::kAccent);
            addColor(left, y + 2.0, std::min(2.0, clipSpan), 40.0,
                     ui::theme::kAccent);
            addColor(std::max(left, right - 2.0), y + 2.0,
                     std::min(2.0, clipSpan), 40.0, ui::theme::kAccent);
        }
    }

    // FC3 -- docs/roadmap/FILMCRAFT-2026-10.md. The Retime controls shown:
    // a band with the clip's speed, and its two ends, where a press takes
    // them (VisibleTimelineRetimeBands).
    for (const TimelineRetimeBand& band : VisibleTimelineRetimeBands(
             shownDocument, (*in.viewport), width, (*in.retime_clip_ids))) {
        const double y = top + band.y;
        if (y >= top + timelineHeight) continue;
        addColor(band.x, y, band.width, band.height, ui::theme::kRetimeBand);
        addColor(band.head_x, y, band.head_width, band.height,
                 ui::theme::kRetimeHandle);
        addColor(band.tail_x, y, band.tail_width, band.height,
                 ui::theme::kRetimeHandle);
        addText(
            band.x + kTimelineEdgeHitWidth + ui::theme::kSpaceXs, y + 1.0,
            band.width - (2.0 * (kTimelineEdgeHitWidth + ui::theme::kSpaceXs)),
            ui::theme::kFontSizeCaption, band.label, ui::theme::kTextClip,
            MetalFontFace::Mono, true);
    }

    // GitHub #330 -- the fade handles of the audio clip under the pointer,
    // of the selected ones and of the one being dragged, with the tools
    // that take them.
    const TimelineTool fadeTool = in.effective_tool;
    if (fadeTool == TimelineTool::Select || fadeTool == TimelineTool::Trim) {
        const std::vector<Ulid>& selectedIds =
            in.interaction->SelectedClipIds();
        for (const TimelineAudioFadeRect& fade : fades) {
            if (!fade.handles) continue;
            const bool dragged =
                in.fade_drag && in.fade_drag->press.clip_id == fade.clip_id;
            if (!dragged && fade.clip_id != in.hovered_clip_id &&
                std::find(selectedIds.begin(), selectedIds.end(),
                          fade.clip_id) == selectedIds.end())
                continue;
            const double y = top + fade.handle_y;
            if (y >= top + timelineHeight) continue;
            addColor(fade.in_handle_x, y, fade.in_handle_width,
                     fade.handle_height, ui::theme::kTimelineFadeHandle);
            addColor(fade.out_handle_x, y, fade.out_handle_width,
                     fade.handle_height, ui::theme::kTimelineFadeHandle);
        }
    }

    // FC4 -- docs/roadmap/FILMCRAFT-2026-10.md. The edit points the trim
    // mode holds, on the zone a press finds them in (VisibleTimelineEdgeMarks):
    // green with media left beyond them, red at its end.
    if (in.trim_session->Active()) {
        for (const TimelineEdgeMarkRect& mark :
             VisibleTimelineEdgeMarks(shownDocument, (*in.viewport), width,
                                      trim::TrimSessionEdgeMarks(
                                          shownDocument, (*in.trim_session)))) {
            const double y = top + mark.y;
            if (y >= top + timelineHeight) continue;
            addColor(mark.x, y + ui::theme::kTimelineClipBodyTop, mark.width,
                     ui::theme::kTimelineClipBodyHeight,
                     mark.at_limit ? ui::theme::kTrimPointAtLimit
                                   : ui::theme::kTrimPoint);
        }
    }

    // Locked lanes stay visible but receive a sparse diagonal safety hatch
    // above their clips. Axis-aligned stair steps keep this compatible with
    // the timeline's rectangle-only Metal display list.
    for (size_t index = 0; index < tracks.size(); ++index) {
        if (!tracks[index]->locked) continue;
        const double y =
            top + kTimelineRulerHeight + index * in.viewport->track_height;
        const double height = in.viewport->track_height;
        if (y >= top + timelineHeight) continue;
        add(in.viewport->header_width, y, width - in.viewport->header_width,
            height, 0.0f, 0.0f, 0.0f, 0.26f);
        for (double baseX = in.viewport->header_width - height; baseX < width;
             baseX += 16.0) {
            for (double offset = 0.0; offset < height; offset += 3.0) {
                const double x = baseX + offset;
                if (x >= in.viewport->header_width && x < width)
                    add(x, y + offset, std::min(4.0, width - x), 1.5,
                        ui::theme::kTextTertiary.r, ui::theme::kTextTertiary.g,
                        ui::theme::kTextTertiary.b, 0.42f);
            }
        }
    }

    // The range belongs to the timeline, not to its empty background. Draw
    // its translucent body after clips so In/Out remains readable across
    // picture, waveform and gaps alike.
    if (in.has_valid_timeline_range) {
        const double rawIn = in.viewport->TimeToX(*in.timeline_in);
        const double rawOut = in.viewport->TimeToX(*in.timeline_out);
        const double left = std::max(in.viewport->header_width, rawIn);
        const double right = std::min(width, rawOut);
        if (right > left)
            add(left, top + kTimelineRulerHeight, right - left,
                std::max(0.0, timelineHeight - kTimelineRulerHeight -
                                  kTimelineZoomBarHeight),
                ui::theme::kAccent.r, ui::theme::kAccent.g,
                ui::theme::kAccent.b, 0.10f);
        if (rawIn >= in.viewport->header_width && rawIn <= width)
            addColor(
                rawIn, top + kTimelineRulerHeight, 1.0,
                timelineHeight - kTimelineRulerHeight - kTimelineZoomBarHeight,
                ui::theme::kMarkIn);
        if (rawOut >= in.viewport->header_width && rawOut <= width)
            addColor(
                rawOut - 1.0, top + kTimelineRulerHeight, 1.0,
                timelineHeight - kTimelineRulerHeight - kTimelineZoomBarHeight,
                ui::theme::kMarkOut);
    }

    for (const DocumentTransition& transition :
         in.document->sequence.transitions) {
        const DocumentTrack* track =
            in.document->FindTrack(transition.track_id);
        if (!track) continue;
        const auto displayTrack =
            std::find(tracks.begin(), tracks.end(), track);
        if (displayTrack == tracks.end()) continue;
        const MediaRate rate = in.document->sequence.frame_rate;
        // R5 -- docs/roadmap/REEL-2026-09.md. Where the transition falls,
        // by the one function the monitor, the export and Validate ask.
        // This block used to work the split out for itself and to read the
        // right-hand clip to find the cut -- which drew nothing at all for
        // a colour dip that closes a track, since that one has no
        // right-hand clip.
        TransitionSpan span;
        std::string spanError;
        if (!ResolveTransitionSpan(in.document->sequence, transition, span,
                                   spanError))
            continue;
        const RationalTime cutTime{span.cut_frame * rate.den, rate.num};
        const RationalTime pre{span.pre_frames * rate.den, rate.num};
        const RationalTime post{span.post_frames * rate.den, rate.num};
        double leftX = in.viewport->TimeToX(cutTime.sub(pre));
        double rightX = in.viewport->TimeToX(cutTime.add(post));
        leftX = std::max(leftX, in.viewport->header_width);
        rightX = std::min(rightX, width);
        if (rightX <= leftX) continue;
        const double y = top + kTimelineRulerHeight +
                         std::distance(tracks.begin(), displayTrack) *
                             in.viewport->track_height +
                         4.0;
        const double height = std::max(8.0, in.viewport->track_height - 8.0);
        addColor(leftX, y, rightX - leftX, height,
                 ui::theme::WithAlpha(ui::theme::kSurfaceControlHi, 0.72f));
        addColor(leftX, y, rightX - leftX, 2.0, ui::theme::kAccent);
        addColor(leftX, y + height - 2.0, rightX - leftX, 2.0,
                 ui::theme::kAccent);
        const double cutX = in.viewport->TimeToX(cutTime);
        addColor(cutX - 1.0, y, 2.0, height, ui::theme::kTextPrimary);
    }

    // EF1 -- docs/roadmap/EFFETS-2026-10.md. The cut a dragged effect would
    // land on: a band of the accent either side of it, on its track.
    // EF3 -- a clip effect lands on a whole clip: the band covers it, edged
    // with the accent above and below.
    if (in.effect_drop_target && in.effect_drop_target->valid &&
        !in.effect_drop_target->clip_id.empty()) {
        const ui::effects::DropTarget& target = *in.effect_drop_target;
        const auto found = std::find_if(tracks.begin(), tracks.end(),
                                        [&](const DocumentTrack* track) {
                                            return track->id == target.track_id;
                                        });
        const DocumentClip* clip = in.document->FindClip(target.clip_id);
        if (found != tracks.end() && clip != nullptr) {
            const double left =
                std::max(in.viewport->header_width,
                         in.viewport->TimeToX(clip->timeline_in));
            const double right = std::min(
                width,
                in.viewport->TimeToX(clip->timeline_in.add(clip->duration)));
            const double y = top + kTimelineRulerHeight +
                             std::distance(tracks.begin(), found) *
                                 in.viewport->track_height +
                             2.0;
            const double height =
                std::max(8.0, in.viewport->track_height - 4.0);
            if (right > left) {
                addColor(left, y, right - left, height,
                         ui::theme::WithAlpha(ui::theme::kAccent, 0.35f));
                addColor(left, y, right - left, 2.0, ui::theme::kAccent);
                addColor(left, y + height - 2.0, right - left, 2.0,
                         ui::theme::kAccent);
            }
        }
    } else if (in.effect_drop_target && in.effect_drop_target->valid) {
        const ui::effects::DropTarget& target = *in.effect_drop_target;
        const auto found = std::find_if(tracks.begin(), tracks.end(),
                                        [&](const DocumentTrack* track) {
                                            return track->id == target.track_id;
                                        });
        const double cutX = in.viewport->TimeToX(target.cut);
        if (found != tracks.end() && cutX >= in.viewport->header_width &&
            cutX <= width) {
            const double y = top + kTimelineRulerHeight +
                             std::distance(tracks.begin(), found) *
                                 in.viewport->track_height +
                             2.0;
            const double height =
                std::max(8.0, in.viewport->track_height - 4.0);
            constexpr double kBand = 12.0;
            const double left =
                target.left_clip_id.empty() ? cutX : cutX - kBand;
            const double right =
                target.right_clip_id.empty() ? cutX : cutX + kBand;
            addColor(left, y, right - left, height,
                     ui::theme::WithAlpha(ui::theme::kAccent, 0.35f));
            addColor(cutX - 1.0, y, 2.0, height, ui::theme::kAccent);
        }
    }

    if (in.lasso_dragging) {
        const double left =
            std::max(in.viewport->header_width,
                     std::min(in.lasso_start_x, in.lasso_current_x));
        const double right =
            std::min(width, std::max(in.lasso_start_x, in.lasso_current_x));
        const double lassoTop =
            std::max(kTimelineRulerHeight,
                     std::min(in.lasso_start_y, in.lasso_current_y));
        const double bottom = std::max(in.lasso_start_y, in.lasso_current_y);
        if (right > left && bottom > lassoTop) {
            addColor(left, top + lassoTop, right - left, bottom - lassoTop,
                     ui::theme::WithAlpha(ui::theme::kAccent, 0.12f));
            addColor(left, top + lassoTop, right - left, 1.0,
                     ui::theme::kAccent);
            addColor(left, top + bottom - 1.0, right - left, 1.0,
                     ui::theme::kAccent);
            addColor(left, top + lassoTop, 1.0, bottom - lassoTop,
                     ui::theme::kAccent);
            addColor(right - 1.0, top + lassoTop, 1.0, bottom - lassoTop,
                     ui::theme::kAccent);
        }
    }

    if (in.interaction->SnapGuideTime()) {
        const double snapX =
            in.viewport->TimeToX(*in.interaction->SnapGuideTime());
        if (snapX >= in.viewport->header_width && snapX <= width)
            addColor(
                snapX, top + kTimelineRulerHeight, 1.0,
                timelineHeight - kTimelineRulerHeight - kTimelineZoomBarHeight,
                ui::theme::kAccent);
    }
    // Out of lot -- the author, 29 October 2026: the trim's frame counter, as
    // in Resolve, on a plate just above the trimmed clip, beside its moved edge
    // (inside the clip when the clip is on the top track).
    if (const auto readout = TimelineTrimReadoutFor(
            (*in.document), in.interaction->TrimPreview(), in.playhead_rate)) {
        const auto rect = std::find_if(
            clips.begin(), clips.end(), [&](const TimelineClipRect& clip) {
                return clip.clip_id == readout->clip_id;
            });
        const double edgeX = in.viewport->TimeToX(readout->edge);
        if (rect != clips.end() && edgeX >= in.viewport->header_width &&
            edgeX <= width) {
            const double plateWidth =
                10.0 + 7.0 * static_cast<double>(readout->text.size());
            const double plateHeight = 16.0;
            const double plateX =
                std::min(edgeX + 6.0, width - plateWidth - 2.0);
            double plateY = top + rect->y - plateHeight - 2.0;
            if (plateY < top + kTimelineRulerHeight)
                plateY = top + rect->y + 3.0;
            add(plateX, plateY, plateWidth, plateHeight, 0.0f, 0.0f, 0.0f,
                0.82f);
            addText(plateX + 5.0, plateY + 2.0, plateWidth - 10.0,
                    ui::theme::kFontSizeSmall, readout->text,
                    ui::theme::kTextClip, MetalFontFace::Mono, true);
        }
    }
    if (in.cut_preview_x && in.cut_preview_y) {
        addColor(*in.cut_preview_x - 1.0, top + *in.cut_preview_y, 2.0,
                 in.viewport->track_height, ui::theme::kAccent);
    }

    // D2 -- docs/roadmap/DIALOGUE-2026-10.md. Sequence markers. Each flag is
    // the rectangle the pointer hits (VisibleTimelineMarkers, TimelineView.h):
    // only its colour and the note beside it are decided here. The note is
    // shown for the flag under the pointer, else for the marker the paused
    // playhead rests on -- not during playback, where it would flash by.
    const DocumentMarker* playheadMarker =
        in.playback_direction == 0
            ? TimelineMarkerCoveringPlayhead(
                  (*in.document), in.requested_position, in.playhead_rate)
            : nullptr;
    // D6 -- a marker being stretched is drawn as the gesture has it.
    std::optional<TimelineMarkerPreview> stretched;
    if (in.marker_stretch && in.marker_stretch->reached)
        stretched = TimelineMarkerPreview{in.marker_stretch->marker_id,
                                          in.marker_stretch->reached->time,
                                          in.marker_stretch->reached->duration};
    const Ulid notedMarkerId = !in.hovered_marker_id.empty()
                                   ? in.hovered_marker_id
                               : playheadMarker ? playheadMarker->id
                                                : Ulid{};
    for (const TimelineMarkerFlag& flag : VisibleTimelineMarkers(
             (*in.document), (*in.viewport), width, stretched)) {
        const DocumentMarker* marker = in.document->FindMarker(flag.marker_id);
        if (!marker) continue;
        const ui::theme::Color color = MarkerDisplayColor(*marker);
        // A filled tag on the ruler, its point on the marker's time, and
        // nothing in the tracks: a line down through them read as a second
        // playhead (Visual QA of 1 October 2026). Rows that would cross the
        // track headers are cut at their edge.
        const double headerEdge = in.viewport->header_width;
        const auto addClipped = [&](double x, double y, double w, double h,
                                    ui::theme::Color fill) {
            // D6 -- a span may also run past the right edge of the view.
            const double left = std::max(headerEdge, x);
            const double right = std::min(width, x + w);
            addColor(left, y, right - left, h, fill);
        };
        const auto shape = TimelineMarkerFlagShape(flag);
        for (const TimelineMarkerFlagRow& row : shape)
            addClipped(row.x, top + row.y, row.width, row.height, color);
        if (flag.marker_id == in.hovered_marker_id && !shape.empty())
            addClipped(shape.front().x, top + shape.front().y,
                       shape.front().width, 2.0, ui::theme::kTextPrimary);
        if (flag.marker_id != notedMarkerId) continue;
        constexpr double kNoteHeight = 18.0;
        constexpr double kNoteMaxWidth = 360.0;
        const double room = width - ui::theme::kSpaceXs;
        double noteWidth =
            std::min(kNoteMaxWidth, in.marker_note_width(marker->name) +
                                        2.0 * ui::theme::kSpaceS);
        // Beside the tag; for a span begun left of the view, beside what
        // shows of it.
        const double tagX = std::max(flag.x, in.viewport->header_width);
        double noteX = tagX + ui::theme::kSpaceS;
        // No room on the right of the tag: the note goes to its left.
        if (noteX + noteWidth > room)
            noteX = tagX - ui::theme::kSpaceS - noteWidth;
        noteX =
            std::max(noteX, in.viewport->header_width + ui::theme::kSpaceXs);
        noteWidth = std::min(noteWidth, room - noteX);
        const double noteY = top + kTimelineRulerHeight + ui::theme::kSpaceXs;
        addColor(noteX, noteY, noteWidth, kNoteHeight,
                 ui::theme::kSurfaceControlHi);
        addColor(noteX, noteY, 2.0, kNoteHeight, color);
        addText(noteX + ui::theme::kSpaceS, noteY + 2.0,
                noteWidth - 2.0 * ui::theme::kSpaceS + 1.0,
                ui::theme::kFontSizeSmall, marker->name,
                ui::theme::kTextPrimary, MetalFontFace::Sans, false);
    }

    const double zoomY = top + timelineHeight - kTimelineZoomBarHeight;
    addColor(0.0, zoomY, width, kTimelineZoomBarHeight,
             ui::theme::kSurfaceSunken);
    addColor(0.0, zoomY, in.viewport->header_width, kTimelineZoomBarHeight,
             ui::theme::kSurfacePanel);
    // While a drag holds the bar, the rail keeps the span the drag began
    // with: the handle stays under the pointer (TimelineView.h).
    const TimelineZoomBarGeometry zoomGeometry =
        CalculateTimelineZoomBarGeometry(
            (*in.viewport), in.duration, width,
            in.zoom_bar_drag ? in.zoom_bar_drag->initial_geometry.ruler_seconds
                             : 0.0);
    if (zoomGeometry.thumb_width > 0.0) {
        addV(zoomGeometry.thumb_x, zoomY + 2.0, zoomGeometry.thumb_width, 10.0,
             ui::theme::kSurfaceControlHi, ui::theme::kSurfaceControl);
        addColor(zoomGeometry.thumb_x, zoomY + 2.0, zoomGeometry.handle_width,
                 10.0, ui::theme::kTextTertiary);
        addColor(zoomGeometry.thumb_x + zoomGeometry.thumb_width -
                     zoomGeometry.handle_width,
                 zoomY + 2.0, zoomGeometry.handle_width, 10.0,
                 ui::theme::kTextTertiary);
        if (in.zoom_bar_drag)
            addColor(zoomGeometry.thumb_x, zoomY, zoomGeometry.thumb_width, 2.0,
                     ui::theme::kAccent);
    }

    const double playheadX = in.viewport->TimeToX(in.requested_position);
    if (playheadX >= in.viewport->header_width && playheadX <= width) {
        addColor(
            playheadX, top + kTimelineToolbarHeight, 1.0,
            timelineHeight - kTimelineToolbarHeight - kTimelineZoomBarHeight,
            ui::theme::kAccent);
    }
    return data;
}
