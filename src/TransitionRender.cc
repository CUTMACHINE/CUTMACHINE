#include "TransitionRender.h"

#include <algorithm>
#include <stdexcept>

namespace {

int HexDigit(char character) {
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
    }
    return -1;
}

// A fraction of frames is already in lowest terms often enough not to
// matter, but the document compares these and the format writes them, so
// they are reduced: 6/12 and 1/2 are the same veil.
EffectParamValue Fraction(int64_t numerator, int64_t denominator) {
    if (denominator <= 0) {
        return {0, 1};
    }
    numerator = std::clamp<int64_t>(numerator, 0, denominator);
    int64_t left = numerator;
    int64_t right = denominator;
    while (right != 0) {
        const int64_t remainder = left % right;
        left = right;
        right = remainder;
    }
    const int64_t divisor = left == 0 ? 1 : left;
    return {static_cast<int32_t>(numerator / divisor),
            static_cast<int32_t>(denominator / divisor)};
}

}  // namespace

bool ParseTransitionColor(const std::string& text, TransitionColor& color,
                          std::string& error) {
    if (text.size() != 7 || text[0] != '#') {
        error = "colour '" + text + "' must be spelled #rrggbb";
        return false;
    }
    int32_t channels[3] = {0, 0, 0};
    for (int index = 0; index < 3; ++index) {
        const int high = HexDigit(text[1 + 2 * index]);
        const int low = HexDigit(text[2 + 2 * index]);
        if (high < 0 || low < 0) {
            error = "colour '" + text + "' must be spelled #rrggbb";
            return false;
        }
        channels[index] = high * 16 + low;
    }
    color = {channels[0], channels[1], channels[2]};
    error.clear();
    return true;
}

std::string TransitionColorHex(const TransitionColor& color) {
    static const char* kDigits = "0123456789abcdef";
    std::string hex(6, '0');
    const int32_t channels[3] = {color.red, color.green, color.blue};
    for (size_t index = 0; index < 3; ++index) {
        const int32_t value = std::clamp(channels[index], 0, 255);
        hex[(2 * index)] = kDigits[value / 16];
        hex[(2 * index) + 1] = kDigits[value % 16];
    }
    return hex;
}

bool ResolveTransitionSpan(const DocumentSequence& sequence,
                           const DocumentTransition& transition,
                           TransitionSpan& span, std::string& error) {
    span = {};
    if (sequence.frame_rate.num <= 0 || sequence.frame_rate.den <= 0) {
        error = "the sequence has no usable frame rate";
        return false;
    }
    const DocumentTrack* track = nullptr;
    for (const DocumentTrack& candidate : sequence.tracks) {
        if (candidate.id == transition.track_id) {
            track = &candidate;
        }
    }
    if (track == nullptr) {
        error = "names a track that does not exist";
        return false;
    }
    const DocumentClip* left = nullptr;
    const DocumentClip* right = nullptr;
    size_t leftIndex = track->clips.size();
    size_t rightIndex = track->clips.size();
    for (size_t index = 0; index < track->clips.size(); ++index) {
        if (!transition.left_clip_id.empty() &&
            track->clips[index].id == transition.left_clip_id) {
            left = &track->clips[index];
            leftIndex = index;
        }
        if (!transition.right_clip_id.empty() &&
            track->clips[index].id == transition.right_clip_id) {
            right = &track->clips[index];
            rightIndex = index;
        }
    }
    if ((left == nullptr && !transition.left_clip_id.empty()) ||
        (right == nullptr && !transition.right_clip_id.empty())) {
        error = "names a clip that is not on its track";
        return false;
    }
    if (left == nullptr && right == nullptr) {
        error = "names no clip at all";
        return false;
    }
    if (left != nullptr && right != nullptr && rightIndex != leftIndex + 1) {
        error = "must reference adjacent clips in edit order";
        return false;
    }
    try {
        // The cut: where the two clips meet, or the edge of the only clip
        // there is. An opening fade cuts on that clip's first frame, a
        // closing one just past its last -- so that in both cases the veil
        // is full on the cut frame and the alignment says which side the
        // frames fall on.
        RationalTime cut;
        if (left != nullptr && right != nullptr) {
            cut = left->timeline_in.add(left->duration);
            if (cut != right->timeline_in) {
                error = "clips do not share a cut";
                return false;
            }
        } else if (right != nullptr) {
            cut = right->timeline_in;
        } else {
            cut = left->timeline_in.add(left->duration);
        }
        const int64_t frames = transition.duration.to_frames(
            sequence.frame_rate.num, sequence.frame_rate.den);
        const RationalTime exact{
            frames * static_cast<int64_t>(sequence.frame_rate.den),
            sequence.frame_rate.num};
        if (frames <= 0 || exact != transition.duration) {
            error = "duration must contain whole sequence frames";
            return false;
        }
        const RationalTime cutExact{
            cut.to_frames(sequence.frame_rate.num, sequence.frame_rate.den) *
                static_cast<int64_t>(sequence.frame_rate.den),
            sequence.frame_rate.num};
        if (cutExact != cut) {
            error = "sits on a cut that is not a whole sequence frame";
            return false;
        }
        span.cut_frame =
            cut.to_frames(sequence.frame_rate.num, sequence.frame_rate.den);
        if (transition.alignment == TransitionAlignment::Center) {
            span.pre_frames = frames / 2;
            span.post_frames = frames - span.pre_frames;
        } else if (transition.alignment == TransitionAlignment::StartAtCut) {
            span.post_frames = frames;
        } else {
            span.pre_frames = frames;
        }
    } catch (const std::exception& exception) {
        error = std::string("has invalid rational time arithmetic: ") +
                exception.what();
        return false;
    }
    error.clear();
    return true;
}

EffectParamValue CrossDissolveProgress(int64_t index, int64_t frames) {
    if (frames <= 0) {
        return {1, 1};
    }
    return Fraction(index, frames);
}

EffectParamValue ColorVeilAlpha(const TransitionSpan& span, int64_t index) {
    const int64_t frames = span.frames();
    if (frames <= 0 || index < 0 || index >= frames) {
        return {0, 1};
    }
    if (index < span.pre_frames) {
        return Fraction(index, span.pre_frames);
    }
    return Fraction(frames - index, span.post_frames);
}

std::optional<ResolvedColorVeil> ResolveTrackColorVeil(
    const DocumentSequence& sequence, const Ulid& trackId, int64_t frame) {
    for (const DocumentTransition& transition : sequence.transitions) {
        if (transition.track_id != trackId) {
            continue;
        }
        if (!TransitionIsColorDip(transition)) {
            continue;
        }
        TransitionSpan span;
        std::string error;
        if (!ResolveTransitionSpan(sequence, transition, span, error)) {
            continue;
        }
        if (!span.covers(frame)) {
            continue;
        }
        TransitionColor color;
        if (!ParseTransitionColor(transition.color, color, error)) {
            continue;
        }
        ResolvedColorVeil veil;
        veil.transition_id = transition.id;
        veil.color = color;
        veil.alpha = ColorVeilAlpha(span, frame - span.first_frame());
        return veil;
    }
    return std::nullopt;
}

int32_t TransitionAlphaCode(const EffectParamValue& alpha) {
    if (alpha.den <= 0) {
        return 0;
    }
    // Nearest, ties away from zero, like every other rounding of this lot.
    const int64_t code =
        (2LL * 255 * alpha.num + alpha.den) / (2LL * alpha.den);
    return static_cast<int32_t>(std::clamp<int64_t>(code, 0, 255));
}

float TransitionAlphaFloat(const EffectParamValue& alpha) {
    if (alpha.den <= 0) {
        return 0.0f;
    }
    return std::clamp(
        static_cast<float>(alpha.num) / static_cast<float>(alpha.den), 0.0f,
        1.0f);
}
