// CA1, CA2 -- docs/roadmap/CADRAGE-2026-10.md. See SubjectTracking.h for the
// rule; this file is its arithmetic, and the bridge to R1 and R2's
// operations. No AppKit, no FFmpeg, no finder.

#include "SubjectTracking.h"

#include "ClipFraming.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <numeric>
#include <sstream>

namespace {

double Fraction(const EffectParamValue& value) {
    return static_cast<double>(value.num) / static_cast<double>(value.den);
}

bool FractionInside(const EffectParamValue& value, double low, double high,
                    bool lowOpen, const char* name, std::string& error) {
    if (value.den <= 0) {
        error = std::string("'") + name + "' needs a positive denominator";
        return false;
    }
    const double number = Fraction(value);
    if (number < low || (lowOpen && number <= low) || number > high) {
        std::ostringstream text;
        text << "'" << name << "' must lie in " << (lowOpen ? "(" : "[") << low
             << ", " << high << "]";
        error = text.str();
        return false;
    }
    return true;
}

// The ratio of two non-negative exact durations, the one dimensionless
// quantity allowed to become a double here (as in Keyframes.h).
double Ratio(const RationalTime& numerator, const RationalTime& denominator) {
    const __int128 top = static_cast<__int128>(numerator.value) *
                         static_cast<__int128>(denominator.rate);
    const __int128 bottom = static_cast<__int128>(denominator.value) *
                            static_cast<__int128>(numerator.rate);
    if (bottom == 0) return 0.0;
    return static_cast<double>(top) / static_cast<double>(bottom);
}

RationalTime FrameTime(int64_t frame, const MediaRate& rate) {
    return RationalTime(frame * rate.den, rate.num);
}

EffectParamValue Reduced(int64_t numerator, int64_t denominator) {
    const int64_t divisor = std::gcd(std::llabs(numerator), denominator);
    if (divisor > 1) {
        numerator /= divisor;
        denominator /= divisor;
    }
    if (numerator == 0) denominator = 1;
    return EffectParamValue{static_cast<int32_t>(numerator),
                            static_cast<int32_t>(denominator)};
}

struct Subject {
    bool present = false;
    double centre_x = 0.0;  // displayed source fraction
    double centre_y = 0.0;
    double width = 0.0;  // displayed source pixels
};

Subject FaceSubject(const ColorMeasureRegion& face, int64_t sourceWidth) {
    Subject subject;
    subject.present = true;
    const double x = Fraction(face.x);
    const double y = Fraction(face.y);
    const double width = Fraction(face.width);
    const double height = Fraction(face.height);
    subject.centre_x = std::clamp(x + width / 2.0, 0.0, 1.0);
    subject.centre_y = std::clamp(y + height / 2.0, 0.0, 1.0);
    subject.width = width * static_cast<double>(sourceWidth);
    return subject;
}

// The distance between two places of the displayed picture, in its pixels:
// a face width is measured in pixels, so the gate compares like with like.
double PixelDistance(const Subject& left, const Subject& right,
                     int64_t sourceWidth, int64_t sourceHeight) {
    const double dx =
        (left.centre_x - right.centre_x) * static_cast<double>(sourceWidth);
    const double dy =
        (left.centre_y - right.centre_y) * static_cast<double>(sourceHeight);
    return std::hypot(dx, dy);
}

double Median3(double a, double b, double c) {
    return std::max(std::min(a, b), std::min(std::max(a, b), c));
}

// The operator, on one axis. `target` is where the frame would have to be,
// frame by frame, to put the subject on its place, already inside the room.
// Returns the frame's position at every frame.
std::vector<double> OperateAxis(const std::vector<double>& target,
                                double deadZone, double settle, double maxSpeed,
                                double maxAcceleration, double frameDuration,
                                double room, int64_t& moves) {
    std::vector<double> path(target.size(), 0.0);
    moves = 0;
    if (target.empty()) return path;
    double position = target.front();
    double velocity = 0.0;
    bool moving = false;
    const double step = maxAcceleration * frameDuration;
    for (size_t frame = 0; frame < target.size(); ++frame) {
        const double error = target[frame] - position;
        if (!moving && std::fabs(error) > deadZone) {
            moving = true;
            ++moves;
        }
        if (moving) {
            const bool settled = std::fabs(error) <= settle;
            // Past the goal (a subject that stopped faster than the frame
            // can brake) or on it, the frame brakes at the acceleration
            // bound and the move ends where it comes to rest: it does not
            // come back for an overshoot the dead zone still holds. Never
            // a reversal inside a move, never a tremble around the goal,
            // and never a stop sharper than the acceleration allows.
            const bool turning =
                velocity != 0.0 && (error > 0.0) != (velocity > 0.0);
            if (settled || turning) {
                if (std::fabs(velocity) <= step) {
                    moving = false;
                    velocity = 0.0;
                } else {
                    velocity -= velocity > 0.0 ? step : -step;
                    position += velocity * frameDuration;
                }
            } else {
                // The braking curve: the fastest speed from which the
                // acceleration can still stop the frame on the goal.
                const double reach =
                    std::sqrt(2.0 * maxAcceleration * std::fabs(error));
                double wanted = std::min(maxSpeed, reach);
                if (error < 0.0) wanted = -wanted;
                velocity += std::clamp(wanted - velocity, -step, step);
                velocity = std::clamp(velocity, -maxSpeed, maxSpeed);
                position += velocity * frameDuration;
            }
        }
        if (position > room) {
            position = room;
            velocity = 0.0;
        } else if (position < -room) {
            position = -room;
            velocity = 0.0;
        }
        path[frame] = position;
    }
    return path;
}

// The fewest frames whose linear interpolation stays within `tolerance` of
// `path` (Ramer-Douglas-Peucker), always the first and the last.
std::vector<size_t> ReduceToKeys(const std::vector<int64_t>& path,
                                 int64_t tolerance) {
    std::vector<bool> keep(path.size(), false);
    keep.front() = true;
    keep.back() = true;
    std::vector<std::pair<size_t, size_t>> spans = {{0, path.size() - 1}};
    while (!spans.empty()) {
        const auto [first, last] = spans.back();
        spans.pop_back();
        if (last <= first + 1) continue;
        // Exact: the line between two keys at an intermediate frame is a
        // rational, compared by cross-multiplication.
        const __int128 length = static_cast<__int128>(last - first);
        __int128 worst = 0;
        size_t worstAt = first;
        for (size_t frame = first + 1; frame < last; ++frame) {
            const __int128 line =
                static_cast<__int128>(path[first]) * length +
                static_cast<__int128>(path[last] - path[first]) *
                    static_cast<__int128>(frame - first);
            __int128 deviation =
                static_cast<__int128>(path[frame]) * length - line;
            if (deviation < 0) deviation = -deviation;
            if (deviation > worst) {
                worst = deviation;
                worstAt = frame;
            }
        }
        if (worst > static_cast<__int128>(tolerance) * length) {
            keep[worstAt] = true;
            spans.push_back({first, worstAt});
            spans.push_back({worstAt, last});
        }
    }
    std::vector<size_t> keys;
    for (size_t frame = 0; frame < keep.size(); ++frame)
        if (keep[frame]) keys.push_back(frame);
    return keys;
}

}  // namespace

bool FillPositionAxis(const std::vector<double>& operated,
                      int64_t roomNumerator, int64_t tolerance,
                      const MediaRate& rate, SubjectTrackingAxis& axis,
                      std::string& error) {
    const int64_t denominator = kAnimationSampleDenominator;
    axis.path.resize(operated.size());
    for (size_t frame = 0; frame < operated.size(); ++frame) {
        const int64_t value = static_cast<int64_t>(
            std::llround(operated[frame] * static_cast<double>(denominator)));
        axis.path[frame] = std::clamp(value, -roomNumerator, roomNumerator);
    }
    axis.room = Reduced(roomNumerator, denominator);
    const bool still =
        std::all_of(axis.path.begin(), axis.path.end(),
                    [&](int64_t value) { return value == axis.path.front(); });
    if (still) {
        axis.animated = false;
        axis.constant = Reduced(axis.path.front(), denominator);
        axis.keyframes.clear();
        return true;
    }
    const std::vector<size_t> keys = ReduceToKeys(axis.path, tolerance);
    if (keys.size() > kMaxClipKeyframes) {
        error = "the followed framing needs " + std::to_string(keys.size()) +
                " keys on one axis, more than the " +
                std::to_string(kMaxClipKeyframes) +
                " a clip may carry: raise 'tolerance' or split the clip";
        return false;
    }
    axis.animated = true;
    axis.constant = Reduced(axis.path.front(), denominator);
    axis.keyframes.clear();
    for (size_t frame : keys) {
        ClipKeyframe keyframe;
        keyframe.time = FrameTime(static_cast<int64_t>(frame), rate);
        keyframe.value = Reduced(axis.path[frame], denominator);
        keyframe.interpolation = KeyframeInterpolation::Linear;
        axis.keyframes.push_back(keyframe);
    }
    return true;
}

// The picture's size against the frame, the room it leaves, both from R1's
// own rule (ClipFraming.h): sw = Wd (1 - l - r), s0 = max(W/sw, H/sh) in
// fill, times the scale; the room on each side is half of what overruns,
// as a fraction of the frame.
FollowedFramingRoom FramingRoomOf(const SubjectTrackingInput& input) {
    const ClipFraming& framing = input.framing;
    const double keptWidth =
        static_cast<double>(input.source_width) *
        (1.0 - Fraction(framing.crop_left) - Fraction(framing.crop_right));
    const double keptHeight =
        static_cast<double>(input.source_height) *
        (1.0 - Fraction(framing.crop_top) - Fraction(framing.crop_bottom));
    const double frameWidth = static_cast<double>(input.frame_width);
    const double frameHeight = static_cast<double>(input.frame_height);
    const double scale =
        std::max(frameWidth / keptWidth, frameHeight / keptHeight) *
        Fraction(framing.scale);
    FollowedFramingRoom room;
    room.span_x = keptWidth * scale / frameWidth;
    room.span_y = keptHeight * scale / frameHeight;
    // The room, floored onto the sample grid with a margin far below a
    // pixel, so that R1's rounding of the edges never uncovers one.
    const auto roomOf = [](double span) -> int64_t {
        const double half = (span - 1.0) / 2.0;
        if (half <= 0.0) return 0;
        return std::max<int64_t>(
            0, static_cast<int64_t>(std::floor(
                   half * static_cast<double>(kAnimationSampleDenominator) -
                   1e-6)));
    };
    room.room_x = roomOf(room.span_x);
    room.room_y = roomOf(room.span_y);
    return room;
}

bool SubjectTrackingSettingsAreValid(const SubjectTrackingSettings& settings,
                                     std::string& error) {
    if (!FractionInside(settings.dead_zone_x, 0.0, 0.5, false, "dead_zone_x",
                        error) ||
        !FractionInside(settings.dead_zone_y, 0.0, 0.5, false, "dead_zone_y",
                        error) ||
        !FractionInside(settings.settle, 0.0, 0.5, true, "settle", error) ||
        !FractionInside(settings.max_speed, 0.0, 10.0, true, "max_speed",
                        error) ||
        !FractionInside(settings.max_acceleration, 0.0, 100.0, true,
                        "max_acceleration", error) ||
        !FractionInside(settings.target_x, 0.0, 1.0, false, "target_x",
                        error) ||
        !FractionInside(settings.target_y, 0.0, 1.0, false, "target_y",
                        error) ||
        !FractionInside(settings.gate, 0.0, 100.0, true, "gate", error) ||
        !FractionInside(settings.tolerance, 0.0, 0.1, true, "tolerance", error))
        return false;
    if (settings.has_hint &&
        (!FractionInside(settings.hint_x, 0.0, 1.0, false, "hint.x", error) ||
         !FractionInside(settings.hint_y, 0.0, 1.0, false, "hint.y", error)))
        return false;
    // A settle band wider than the dead zone would end every move before
    // it began.
    if (Fraction(settings.settle) > Fraction(settings.dead_zone_x) &&
        settings.dead_zone_x.num != 0) {
        error = "'settle' must not be wider than 'dead_zone_x'";
        return false;
    }
    if (Fraction(settings.settle) > Fraction(settings.dead_zone_y) &&
        settings.dead_zone_y.num != 0) {
        error = "'settle' must not be wider than 'dead_zone_y'";
        return false;
    }
    if (settings.bridge_gap.rate <= 0 || settings.bridge_gap.value < 0) {
        error = "'bridge_gap' must be a non-negative exact duration";
        return false;
    }
    return true;
}

bool ComputeSubjectTracking(const SubjectTrackingInput& input,
                            const SubjectTrackingSettings& settings,
                            SubjectTrackingResult& result, std::string& error) {
    result = SubjectTrackingResult{};
    error.clear();
    if (!SubjectTrackingSettingsAreValid(settings, error)) return false;
    if (input.source_width <= 0 || input.source_height <= 0 ||
        input.frame_width <= 0 || input.frame_height <= 0) {
        error = "the source and the sequence frame must have a size";
        return false;
    }
    if (input.rate.num <= 0 || input.rate.den <= 0 || input.frame_count <= 0) {
        error = "the clip has no frame to frame";
        return false;
    }
    if (input.framing.mode != FramingMode::Fill) {
        error =
            "a followed framing moves inside what 'fill' cuts: a framing "
            "that fits has nothing to pan across";
        return false;
    }
    if (!ClipFramingIsValid(input.framing, error)) return false;
    for (size_t index = 0; index < input.samples.size(); ++index) {
        const SubjectSample& sample = input.samples[index];
        if (sample.time.rate <= 0 || sample.time.value < 0) {
            error = "sample " + std::to_string(index) +
                    ": its time must be a non-negative exact time";
            return false;
        }
        if (index > 0 && !(input.samples[index - 1].time < sample.time)) {
            error = "the samples must be in strictly increasing time";
            return false;
        }
        // Lenient on purpose: a face the finder lets run past the edge of
        // the picture is still a face (ColorMeasure.cc met one on its
        // fourth run). Only its centre is read, and it is clamped inside.
        for (const ColorMeasureRegion& face : sample.faces) {
            if (face.x.den <= 0 || face.y.den <= 0 || face.width.den <= 0 ||
                face.height.den <= 0 || face.width.num <= 0 ||
                face.height.num <= 0) {
                error = "sample " + std::to_string(index) +
                        ": a face needs positive denominators and a positive "
                        "width and height";
                return false;
            }
        }
    }
    result.samples = static_cast<int64_t>(input.samples.size());

    // The picture's size against the frame and the room it leaves
    // (FramingRoomOf).
    const ClipFraming& framing = input.framing;
    const FollowedFramingRoom room = FramingRoomOf(input);
    const double spanX = room.span_x;
    const double spanY = room.span_y;
    const int64_t roomX = room.room_x;
    const int64_t roomY = room.room_y;
    if (roomX == 0 && roomY == 0) {
        error =
            "the filled picture does not overrun the frame on either axis: "
            "there is nothing to pan across (raise 'scale')";
        return false;
    }

    // Who to follow, sample by sample.
    std::vector<Subject> track(input.samples.size());
    bool haveLast = false;
    Subject last;
    RationalTime lastTime;
    for (size_t index = 0; index < input.samples.size(); ++index) {
        const SubjectSample& sample = input.samples[index];
        if (sample.faces.empty()) continue;
        std::vector<Subject> faces;
        for (const ColorMeasureRegion& face : sample.faces)
            faces.push_back(FaceSubject(face, input.source_width));
        const bool choose =
            !haveLast || sample.time.sub(lastTime) > settings.bridge_gap;
        size_t picked = faces.size();
        if (choose) {
            if (!haveLast && settings.has_hint) {
                Subject hint;
                hint.centre_x = Fraction(settings.hint_x);
                hint.centre_y = Fraction(settings.hint_y);
                double best = 0.0;
                for (size_t face = 0; face < faces.size(); ++face) {
                    const double distance =
                        PixelDistance(faces[face], hint, input.source_width,
                                      input.source_height);
                    if (picked == faces.size() || distance < best) {
                        best = distance;
                        picked = face;
                    }
                }
            } else {
                double best = 0.0;
                for (size_t face = 0; face < faces.size(); ++face) {
                    const double area = Fraction(sample.faces[face].width) *
                                        Fraction(sample.faces[face].height);
                    if (picked == faces.size() || area > best) {
                        best = area;
                        picked = face;
                    }
                }
            }
            if (haveLast) ++result.reacquired;
        } else {
            double best = 0.0;
            for (size_t face = 0; face < faces.size(); ++face) {
                const double distance = PixelDistance(
                    faces[face], last, input.source_width, input.source_height);
                if (picked == faces.size() || distance < best) {
                    best = distance;
                    picked = face;
                }
            }
            if (best > Fraction(settings.gate) * last.width)
                picked = faces.size();
        }
        if (picked == faces.size()) continue;
        if (!haveLast) result.first_subject = sample.faces[picked];
        result.first_subject.present = true;
        track[index] = faces[picked];
        last = faces[picked];
        lastTime = sample.time;
        haveLast = true;
        ++result.samples_with_subject;
    }
    if (result.samples_with_subject == 0) {
        error = "no face found in the " + std::to_string(result.samples) +
                " samples of the clip: nothing to follow";
        return false;
    }

    // One isolated outlier removed: a median of three over consecutive
    // detections of the subject.
    std::vector<size_t> present;
    for (size_t index = 0; index < track.size(); ++index)
        if (track[index].present) present.push_back(index);
    if (present.size() >= 3) {
        std::vector<Subject> filtered = track;
        for (size_t at = 1; at + 1 < present.size(); ++at) {
            const Subject& before = track[present[at - 1]];
            const Subject& here = track[present[at]];
            const Subject& after = track[present[at + 1]];
            filtered[present[at]].centre_x =
                Median3(before.centre_x, here.centre_x, after.centre_x);
            filtered[present[at]].centre_y =
                Median3(before.centre_y, here.centre_y, after.centre_y);
        }
        track = std::move(filtered);
    }

    // The subject's place at every frame: bridged across a short hole,
    // held across a long one.
    const double cropLeft = Fraction(framing.crop_left);
    const double cropTop = Fraction(framing.crop_top);
    const double keptX =
        1.0 - Fraction(framing.crop_left) - Fraction(framing.crop_right);
    const double keptY =
        1.0 - Fraction(framing.crop_top) - Fraction(framing.crop_bottom);
    const double targetX = Fraction(settings.target_x);
    const double targetY = Fraction(settings.target_y);
    const double maxRoomX = static_cast<double>(roomX) /
                            static_cast<double>(kAnimationSampleDenominator);
    const double maxRoomY = static_cast<double>(roomY) /
                            static_cast<double>(kAnimationSampleDenominator);
    std::vector<double> wantedX(static_cast<size_t>(input.frame_count));
    std::vector<double> wantedY(static_cast<size_t>(input.frame_count));
    size_t next = 0;
    for (int64_t frame = 0; frame < input.frame_count; ++frame) {
        const RationalTime time = FrameTime(frame, input.rate);
        while (next < present.size() &&
               input.samples[present[next]].time <= time)
            ++next;
        double centreX = 0.0;
        double centreY = 0.0;
        if (next == 0) {
            centreX = track[present.front()].centre_x;
            centreY = track[present.front()].centre_y;
        } else if (next == present.size()) {
            centreX = track[present.back()].centre_x;
            centreY = track[present.back()].centre_y;
        } else {
            const SubjectSample& before = input.samples[present[next - 1]];
            const SubjectSample& after = input.samples[present[next]];
            const Subject& from = track[present[next - 1]];
            const Subject& to = track[present[next]];
            const RationalTime gap = after.time.sub(before.time);
            if (gap <= settings.bridge_gap) {
                const double progress = Ratio(time.sub(before.time), gap);
                centreX =
                    from.centre_x + (to.centre_x - from.centre_x) * progress;
                centreY =
                    from.centre_y + (to.centre_y - from.centre_y) * progress;
            } else {
                centreX = from.centre_x;
                centreY = from.centre_y;
            }
        }
        // Into the kept source, then into the position that puts that
        // point on its target: x_frame = 1/2 + p + span (u - 1/2).
        const double u = std::clamp((centreX - cropLeft) / keptX, 0.0, 1.0);
        const double v = std::clamp((centreY - cropTop) / keptY, 0.0, 1.0);
        const double positionX = (targetX - 0.5) - spanX * (u - 0.5);
        const double positionY = (targetY - 0.5) - spanY * (v - 0.5);
        if (std::fabs(positionX) > maxRoomX + 1e-9) ++result.x.frames_at_edge;
        if (std::fabs(positionY) > maxRoomY + 1e-9) ++result.y.frames_at_edge;
        wantedX[static_cast<size_t>(frame)] =
            std::clamp(positionX, -maxRoomX, maxRoomX);
        wantedY[static_cast<size_t>(frame)] =
            std::clamp(positionY, -maxRoomY, maxRoomY);
    }

    const double frameDuration = static_cast<double>(input.rate.den) /
                                 static_cast<double>(input.rate.num);
    const std::vector<double> operatedX = OperateAxis(
        wantedX, Fraction(settings.dead_zone_x), Fraction(settings.settle),
        Fraction(settings.max_speed), Fraction(settings.max_acceleration),
        frameDuration, maxRoomX, result.x.moves);
    const std::vector<double> operatedY = OperateAxis(
        wantedY, Fraction(settings.dead_zone_y), Fraction(settings.settle),
        Fraction(settings.max_speed), Fraction(settings.max_acceleration),
        frameDuration, maxRoomY, result.y.moves);
    const int64_t tolerance = std::max<int64_t>(
        1, static_cast<int64_t>(
               std::floor(Fraction(settings.tolerance) *
                          static_cast<double>(kAnimationSampleDenominator))));
    return FillPositionAxis(operatedX, roomX, tolerance, input.rate, result.x,
                            error) &&
           FillPositionAxis(operatedY, roomY, tolerance, input.rate, result.y,
                            error);
}

bool PrepareFollowedFraming(const Document& document, const Ulid& clipId,
                            const std::optional<EffectParamValue>& scale,
                            const std::string& what,
                            SubjectTrackingInput& input, bool& modeChanged,
                            std::string& error) {
    modeChanged = false;
    const DocumentClip* clip = document.FindClip(clipId);
    if (clip == nullptr) {
        error = "unknown clip_id '" + clipId + "'";
        return false;
    }
    const std::string named = "clip '" + clipId + "': ";
    if (IsMotionClip(*clip)) {
        error = named +
                "is a motion clip: it is drawn by the engine, it has no "
                "rush to " +
                what;
        return false;
    }
    if (ClipAnimates(*clip, ClipAnimatedParameter::FramingScale)) {
        error = named +
                "animates its scale: the room a followed framing moves in "
                "would change under it (clear the scale keys first)";
        return false;
    }
    if (!ClipDisplayedSize(document, *clip, input.source_width,
                           input.source_height, error)) {
        error = named + error;
        return false;
    }
    input.frame_width = document.sequence.width;
    input.frame_height = document.sequence.height;
    input.rate = document.sequence.frame_rate;
    if (!ClipFrameCount(*clip, input.rate, input.frame_count, error)) {
        error = named + error;
        return false;
    }
    ClipFraming framing = clip->framing;
    if (framing.mode != FramingMode::Fill) {
        framing.mode = FramingMode::Fill;
        modeChanged = true;
    }
    if (scale) framing.scale = *scale;
    framing = ReduceClipFraming(framing);
    if (!ClipFramingIsValid(framing, error)) {
        error = named + error;
        return false;
    }
    input.framing = framing;
    return true;
}

bool LayFollowedFraming(const Document& document, const Ulid& clipId,
                        ClipFraming framing, const SubjectTrackingAxis& x,
                        const SubjectTrackingAxis& y,
                        const std::function<Ulid()>& newId,
                        std::vector<Operation>& operations,
                        std::string& error) {
    const DocumentClip* clip = document.FindClip(clipId);
    if (clip == nullptr) {
        error = "unknown clip_id '" + clipId + "'";
        return false;
    }
    const std::string named = "clip '" + clipId + "': ";
    framing.position_x = x.constant;
    framing.position_y = y.constant;
    framing = ReduceClipFraming(framing);
    std::vector<ClipParameterAnimation> animation;
    for (const ClipParameterAnimation& track : clip->animation) {
        if (track.parameter == ClipAnimatedParameter::FramingPositionX ||
            track.parameter == ClipAnimatedParameter::FramingPositionY)
            continue;
        animation.push_back(track);
    }
    const auto addAxis = [&](ClipAnimatedParameter parameter,
                             const SubjectTrackingAxis& axis) {
        if (!axis.animated) return;
        ClipParameterAnimation track;
        track.parameter = parameter;
        track.keyframes = axis.keyframes;
        for (ClipKeyframe& keyframe : track.keyframes)
            keyframe.id = newId ? newId() : GenerateUlid();
        animation.push_back(std::move(track));
    };
    addAxis(ClipAnimatedParameter::FramingPositionX, x);
    addAxis(ClipAnimatedParameter::FramingPositionY, y);
    std::stable_sort(animation.begin(), animation.end(),
                     [](const ClipParameterAnimation& left,
                        const ClipParameterAnimation& right) {
                         return static_cast<int>(left.parameter) <
                                static_cast<int>(right.parameter);
                     });
    if (!ClipAnimationIsValid(animation, error)) {
        error = named + error;
        return false;
    }

    // Every frame resolved by the call every renderer makes: not one may
    // leave an edge of the frame uncovered.
    DocumentClip followed = *clip;
    followed.framing = framing;
    followed.animation = animation;
    const int64_t frameWidth = document.sequence.width;
    const int64_t frameHeight = document.sequence.height;
    int64_t frameCount = 0;
    if (!ClipFrameCount(*clip, document.sequence.frame_rate, frameCount,
                        error)) {
        error = named + error;
        return false;
    }
    for (int64_t frame = 0; frame < frameCount; ++frame) {
        ClipFramingGeometry geometry;
        EffectParamValue opacity;
        if (!ResolveClipFramingAtFrame(document, followed, frame, frameWidth,
                                       frameHeight, geometry, opacity, error)) {
            error = named + "frame " + std::to_string(frame) + ": " + error;
            return false;
        }
        if (geometry.uncovered_left != 0 || geometry.uncovered_top != 0 ||
            geometry.uncovered_right != 0 || geometry.uncovered_bottom != 0) {
            error = named + "frame " + std::to_string(frame) +
                    " would leave an edge of the frame uncovered";
            return false;
        }
    }

    operations.push_back(SetClipFramingOperation{{{clipId, framing}}});
    operations.push_back(SetClipAnimationOperation{{{clipId, animation}}});
    return true;
}

bool BuildSubjectTrackingOperations(
    const Document& document, const Ulid& clipId,
    const std::vector<SubjectSample>& samples,
    const SubjectTrackingSettings& settings,
    const std::optional<EffectParamValue>& scale,
    const std::function<Ulid()>& newId, std::vector<Operation>& operations,
    SubjectTrackingResult& result, bool& modeChanged, std::string& error) {
    operations.clear();
    SubjectTrackingInput input;
    if (!PrepareFollowedFraming(document, clipId, scale, "follow a face in",
                                input, modeChanged, error))
        return false;
    input.samples = samples;
    if (!ComputeSubjectTracking(input, settings, result, error)) {
        error = "clip '" + clipId + "': " + error;
        return false;
    }
    return LayFollowedFraming(document, clipId, input.framing, result.x,
                              result.y, newId, operations, error);
}

namespace {

std::string FractionJson(const EffectParamValue& value) {
    return "{\"num\":" + std::to_string(value.num) +
           ",\"den\":" + std::to_string(value.den) + "}";
}

std::string AxisJson(const SubjectTrackingAxis& axis) {
    std::string json = "{\"animated\":";
    json += axis.animated ? "true" : "false";
    json += ",\"keys\":" + std::to_string(axis.keyframes.size());
    json += ",\"moves\":" + std::to_string(axis.moves);
    json += ",\"frames_at_edge\":" + std::to_string(axis.frames_at_edge);
    json += ",\"room\":" + FractionJson(axis.room);
    json += ",\"start\":" + FractionJson(axis.constant);
    json += "}";
    return json;
}

}  // namespace

std::string SerializeSubjectTrackingResult(
    const SubjectTrackingResult& result) {
    std::string json = "{\"samples\":" + std::to_string(result.samples);
    json += ",\"samples_with_subject\":" +
            std::to_string(result.samples_with_subject);
    json += ",\"reacquired\":" + std::to_string(result.reacquired);
    if (result.first_subject.present) {
        json += ",\"first_subject\":{\"x\":" +
                FractionJson(result.first_subject.x) +
                ",\"y\":" + FractionJson(result.first_subject.y) +
                ",\"width\":" + FractionJson(result.first_subject.width) +
                ",\"height\":" + FractionJson(result.first_subject.height) +
                "}";
    }
    json += ",\"x\":" + AxisJson(result.x);
    json += ",\"y\":" + AxisJson(result.y);
    json += "}";
    return json;
}

std::vector<int64_t> SubjectSampleFrames(int64_t frameCount,
                                         int64_t everyFrames) {
    std::vector<int64_t> frames;
    if (frameCount <= 0 || everyFrames <= 0) return frames;
    for (int64_t frame = 0; frame < frameCount; frame += everyFrames)
        frames.push_back(frame);
    if (frames.back() != frameCount - 1) frames.push_back(frameCount - 1);
    return frames;
}
