#include "ClipFraming.h"

#include "SequenceFormat.h"

#include <algorithm>
#include <limits>
#include <sstream>
#include <string>

namespace {

// An exact rational with a 128-bit numerator and denominator, normalized
// after every operation. Nothing here becomes a double: the single step to
// whole pixels is Round below, and it happens once per edge.
struct Fraction {
    __int128 num = 0;
    __int128 den = 1;
};

__int128 Gcd(__int128 left, __int128 right) {
    if (left < 0) {
        left = -left;
    }
    if (right < 0) {
        right = -right;
    }
    while (right != 0) {
        const __int128 remainder = left % right;
        left = right;
        right = remainder;
    }
    return left;
}

// Keeps a fraction in its smallest terms with a positive denominator. The
// products below stay far inside 128 bits for any framing the validator
// accepts (terms at or below 100000, dimensions at or below a few tens of
// thousands of pixels), and reducing after each step is what keeps them
// there over a chain of five.
Fraction Reduce(Fraction value) {
    if (value.den < 0) {
        value.num = -value.num;
        value.den = -value.den;
    }
    const __int128 common = Gcd(value.num, value.den);
    if (common > 1) {
        value.num /= common;
        value.den /= common;
    }
    return value;
}

Fraction Make(__int128 num, __int128 den) { return Reduce({num, den}); }

Fraction Add(const Fraction& left, const Fraction& right) {
    return Make(left.num * right.den + right.num * left.den,
                left.den * right.den);
}

Fraction Sub(const Fraction& left, const Fraction& right) {
    return Make(left.num * right.den - right.num * left.den,
                left.den * right.den);
}

Fraction Mul(const Fraction& left, const Fraction& right) {
    return Make(left.num * right.num, left.den * right.den);
}

// left < right, without dividing: both denominators are positive.
bool Less(const Fraction& left, const Fraction& right) {
    return left.num * right.den < right.num * left.den;
}

// The single rounding: to the nearest whole pixel, ties away from zero.
int64_t Round(const Fraction& value) {
    // Reduce keeps every fraction built here on a positive denominator; the
    // guard is for the reader and the analyser, which cannot see that.
    if (value.den <= 0) {
        return 0;
    }
    const bool negative = value.num < 0;
    const __int128 magnitude = negative ? -value.num : value.num;
    const __int128 rounded = (2 * magnitude + value.den) / (2 * value.den);
    return static_cast<int64_t>(negative ? -rounded : rounded);
}

Fraction Of(const EffectParamValue& value) {
    return Make(value.num, value.den);
}

// A fraction in [0, 1) with bounded terms: what a crop is.
bool CropIsValid(const EffectParamValue& value, const char* what,
                 std::string& error) {
    if (value.den <= 0 || value.num < 0) {
        error = std::string("framing ") + what +
                " must be a fraction of the source with a positive "
                "denominator";
        return false;
    }
    if (value.num >= value.den) {
        error = std::string("framing ") + what +
                " must stay below 1: a crop that leaves no picture is "
                "refused, not clamped";
        return false;
    }
    if (value.num > kMaxFramingTerm || value.den > kMaxFramingTerm) {
        error = std::string("framing ") + what + " has terms above " +
                std::to_string(kMaxFramingTerm);
        return false;
    }
    return true;
}

bool PairIsValid(const EffectParamValue& first, const EffectParamValue& second,
                 const char* what, std::string& error) {
    // first + second < 1, without dividing.
    const __int128 sum = static_cast<__int128>(first.num) * second.den +
                         static_cast<__int128>(second.num) * first.den;
    const __int128 whole = static_cast<__int128>(first.den) * second.den;
    if (sum >= whole) {
        error = std::string("framing crops the whole ") + what +
                " away: the two crops of an axis must leave a picture";
        return false;
    }
    return true;
}

int64_t ClampToEvenFloor(int64_t value, int64_t limit) {
    if (value < 0) {
        value = 0;
    }
    if (value > limit) {
        value = limit;
    }
    return value - (value % 2);
}

int64_t ClampToEvenCeil(int64_t value, int64_t limit) {
    if (value < 0) {
        value = 0;
    }
    if (value % 2 != 0) {
        ++value;
    }
    if (value > limit) {
        value = limit - (limit % 2);
    }
    return value;
}

}  // namespace

ClipFraming ReduceClipFraming(ClipFraming framing) {
    EffectParamValue* values[] = {&framing.scale,      &framing.position_x,
                                  &framing.position_y, &framing.crop_left,
                                  &framing.crop_right, &framing.crop_top,
                                  &framing.crop_bottom};
    for (EffectParamValue* value : values) {
        if (value->den == 0) {
            continue;
        }
        if (value->num == 0) {
            // Zero has one spelling here, 0/1, so that a position or a crop
            // written 0/4 reads back as the default the file leaves out.
            value->den = value->den < 0 ? -1 : 1;
            continue;
        }
        const __int128 common = Gcd(value->num, value->den);
        if (common > 1) {
            value->num = static_cast<int32_t>(value->num / common);
            value->den = static_cast<int32_t>(value->den / common);
        }
    }
    return framing;
}

bool ClipFramingIsValid(const ClipFraming& framing, std::string& error) {
    if (framing.mode != FramingMode::Fit && framing.mode != FramingMode::Fill) {
        error = "framing mode must be 'fit' or 'fill'";
        return false;
    }
    if (framing.scale.num <= 0 || framing.scale.den <= 0) {
        error =
            "framing scale must be a positive fraction spelled with positive "
            "terms; a zero, a negative or a mirror is not a scale";
        return false;
    }
    if (framing.scale.num > kMaxFramingTerm ||
        framing.scale.den > kMaxFramingTerm) {
        error =
            "framing scale has terms above " + std::to_string(kMaxFramingTerm);
        return false;
    }
    const EffectParamValue positions[] = {framing.position_x,
                                          framing.position_y};
    const char* names[] = {"position_x", "position_y"};
    for (int index = 0; index < 2; ++index) {
        const EffectParamValue& value = positions[index];
        if (value.den <= 0) {
            error = std::string("framing ") + names[index] +
                    " must be a fraction of the frame with a positive "
                    "denominator";
            return false;
        }
        if (value.num > kMaxFramingTerm || value.den > kMaxFramingTerm ||
            value.num < -kMaxFramingTerm) {
            error = std::string("framing ") + names[index] +
                    " has terms above " + std::to_string(kMaxFramingTerm);
            return false;
        }
        // Eight frames away from the centre is already far outside; beyond
        // it the offset is a mistake, not an intention.
        if (static_cast<__int128>(value.num) >
                static_cast<__int128>(8) * value.den ||
            static_cast<__int128>(value.num) <
                static_cast<__int128>(-8) * value.den) {
            error = std::string("framing ") + names[index] +
                    " moves the picture more than eight frames off centre";
            return false;
        }
    }
    if (!CropIsValid(framing.crop_left, "crop_left", error) ||
        !CropIsValid(framing.crop_right, "crop_right", error) ||
        !CropIsValid(framing.crop_top, "crop_top", error) ||
        !CropIsValid(framing.crop_bottom, "crop_bottom", error)) {
        return false;
    }
    if (!PairIsValid(framing.crop_left, framing.crop_right, "width", error) ||
        !PairIsValid(framing.crop_top, framing.crop_bottom, "height", error)) {
        return false;
    }
    error.clear();
    return true;
}

bool ResolveClipFraming(const ClipFraming& framing, int64_t sourceWidth,
                        int64_t sourceHeight, int64_t frameWidth,
                        int64_t frameHeight, ClipFramingGeometry& geometry,
                        std::string& error) {
    geometry = {};
    if (!ClipFramingIsValid(framing, error)) {
        return false;
    }
    if (sourceWidth <= 0 || sourceHeight <= 0 || frameWidth <= 0 ||
        frameHeight <= 0) {
        error = "a framing needs a positive source and frame size";
        return false;
    }

    // Step 2 -- the crop, in displayed source pixels.
    const Fraction one = Make(1, 1);
    const Fraction keepX =
        Sub(one, Add(Of(framing.crop_left), Of(framing.crop_right)));
    const Fraction keepY =
        Sub(one, Add(Of(framing.crop_top), Of(framing.crop_bottom)));
    const Fraction cropX = Mul(Make(sourceWidth, 1), Of(framing.crop_left));
    const Fraction cropY = Mul(Make(sourceHeight, 1), Of(framing.crop_top));
    const Fraction cropWidth = Mul(Make(sourceWidth, 1), keepX);
    const Fraction cropHeight = Mul(Make(sourceHeight, 1), keepY);

    // Step 3 -- the mode's scale, and step 4 -- the author's on top of it.
    const Fraction fitX =
        Make(static_cast<__int128>(frameWidth) * cropWidth.den, cropWidth.num);
    const Fraction fitY = Make(
        static_cast<__int128>(frameHeight) * cropHeight.den, cropHeight.num);
    const bool takeX = framing.mode == FramingMode::Fill ? !Less(fitX, fitY)
                                                         : Less(fitX, fitY);
    const Fraction base = takeX ? fitX : fitY;
    const Fraction scale = Mul(base, Of(framing.scale));

    // Step 5 -- the picture, then its centre.
    const Fraction pictureWidth = Mul(cropWidth, scale);
    const Fraction pictureHeight = Mul(cropHeight, scale);
    const Fraction centreX =
        Mul(Make(frameWidth, 1), Add(Make(1, 2), Of(framing.position_x)));
    const Fraction centreY =
        Mul(Make(frameHeight, 1), Add(Make(1, 2), Of(framing.position_y)));
    const Fraction halfWidth = Mul(pictureWidth, Make(1, 2));
    const Fraction halfHeight = Mul(pictureHeight, Make(1, 2));

    // The single rounding, on the four edges rather than on an origin and a
    // size: two pictures that met exactly still meet.
    const int64_t left = Round(Sub(centreX, halfWidth));
    const int64_t right = Round(Add(centreX, halfWidth));
    const int64_t top = Round(Sub(centreY, halfHeight));
    const int64_t bottom = Round(Add(centreY, halfHeight));
    if (right <= left || bottom <= top) {
        error =
            "this framing scales the picture away to nothing at the "
            "sequence's resolution";
        return false;
    }
    geometry.full_x = left;
    geometry.full_y = top;
    geometry.full_width = right - left;
    geometry.full_height = bottom - top;

    const int64_t visibleLeft = left > 0 ? left : 0;
    const int64_t visibleTop = top > 0 ? top : 0;
    const int64_t visibleRight = right < frameWidth ? right : frameWidth;
    const int64_t visibleBottom = bottom < frameHeight ? bottom : frameHeight;
    if (visibleRight <= visibleLeft || visibleBottom <= visibleTop) {
        error =
            "this framing puts the whole picture outside the sequence "
            "frame";
        return false;
    }
    geometry.x = visibleLeft;
    geometry.y = visibleTop;
    geometry.width = visibleRight - visibleLeft;
    geometry.height = visibleBottom - visibleTop;

    geometry.uncovered_left = left > 0 ? left : 0;
    geometry.uncovered_top = top > 0 ? top : 0;
    geometry.uncovered_right = right < frameWidth ? frameWidth - right : 0;
    geometry.uncovered_bottom = bottom < frameHeight ? frameHeight - bottom : 0;
    geometry.covers_frame =
        geometry.uncovered_left == 0 && geometry.uncovered_top == 0 &&
        geometry.uncovered_right == 0 && geometry.uncovered_bottom == 0;

    // The preimage of what is drawn, in source pixels: the cropped source
    // maps linearly onto the *rounded* picture rectangle, so that the
    // region named here is the region the scaler actually reads.
    const auto preimage = [](const Fraction& origin, const Fraction& span,
                             int64_t edge, int64_t pictureOrigin,
                             int64_t pictureSpan) {
        return Add(origin, Mul(span, Make(edge - pictureOrigin, pictureSpan)));
    };
    const int64_t sourceLeft =
        Round(preimage(cropX, cropWidth, visibleLeft, left, right - left));
    const int64_t sourceRight =
        Round(preimage(cropX, cropWidth, visibleRight, left, right - left));
    const int64_t sourceTop =
        Round(preimage(cropY, cropHeight, visibleTop, top, bottom - top));
    const int64_t sourceBottom =
        Round(preimage(cropY, cropHeight, visibleBottom, top, bottom - top));

    // On the even grid a subsampled plane can be cut on, outwards, so the
    // region never shrinks to nothing on a thumbnail-sized frame.
    const int64_t evenLeft = ClampToEvenFloor(sourceLeft, sourceWidth - 2);
    const int64_t evenRight = ClampToEvenCeil(sourceRight, sourceWidth);
    const int64_t evenTop = ClampToEvenFloor(sourceTop, sourceHeight - 2);
    const int64_t evenBottom = ClampToEvenCeil(sourceBottom, sourceHeight);
    geometry.source_x = evenLeft;
    geometry.source_y = evenTop;
    geometry.source_width = evenRight > evenLeft ? evenRight - evenLeft : 2;
    geometry.source_height = evenBottom > evenTop ? evenBottom - evenTop : 2;
    if (geometry.source_x + geometry.source_width > sourceWidth) {
        geometry.source_width = sourceWidth - geometry.source_x;
    }
    if (geometry.source_y + geometry.source_height > sourceHeight) {
        geometry.source_height = sourceHeight - geometry.source_y;
    }

    // R2 -- the crop alone, on the same even grid and for the same reason:
    // a subsampled plane cannot be cut at an odd column.
    const int64_t cropLeft = ClampToEvenFloor(Round(cropX), sourceWidth - 2);
    const int64_t cropRight =
        ClampToEvenCeil(Round(Add(cropX, cropWidth)), sourceWidth);
    const int64_t cropTop = ClampToEvenFloor(Round(cropY), sourceHeight - 2);
    const int64_t cropBottom =
        ClampToEvenCeil(Round(Add(cropY, cropHeight)), sourceHeight);
    geometry.crop_x = cropLeft;
    geometry.crop_y = cropTop;
    geometry.crop_width = cropRight > cropLeft ? cropRight - cropLeft : 2;
    geometry.crop_height = cropBottom > cropTop ? cropBottom - cropTop : 2;
    if (geometry.crop_x + geometry.crop_width > sourceWidth) {
        geometry.crop_width = sourceWidth - geometry.crop_x;
    }
    if (geometry.crop_y + geometry.crop_height > sourceHeight) {
        geometry.crop_height = sourceHeight - geometry.crop_y;
    }

    geometry.is_identity = geometry.source_x == 0 && geometry.source_y == 0 &&
                           geometry.source_width == sourceWidth &&
                           geometry.source_height == sourceHeight &&
                           geometry.x == 0 && geometry.y == 0 &&
                           geometry.width == frameWidth &&
                           geometry.height == frameHeight;
    error.clear();
    return true;
}

bool ResolveClipFramingReduced(const ClipFraming& framing, int64_t sourceWidth,
                               int64_t sourceHeight, int64_t sequenceWidth,
                               int64_t sequenceHeight, int64_t frameWidth,
                               int64_t frameHeight,
                               ClipFramingGeometry& geometry,
                               std::string& error) {
    if (frameWidth <= 0 || frameHeight <= 0) {
        error = "the reduced frame must have a positive size";
        return false;
    }
    if (!ResolveClipFraming(framing, sourceWidth, sourceHeight, sequenceWidth,
                            sequenceHeight, geometry, error)) {
        return false;
    }
    if (frameWidth == sequenceWidth && frameHeight == sequenceHeight) {
        return true;
    }
    const auto scale = [](int64_t value, int64_t to, int64_t from) {
        return Round(Make(static_cast<__int128>(value) * to, from));
    };
    const auto span = [&](int64_t start, int64_t length, int64_t to,
                          int64_t from, int64_t& scaledStart,
                          int64_t& scaledLength) {
        scaledStart = scale(start, to, from);
        const int64_t end = scale(start + length, to, from);
        scaledLength = end > scaledStart ? end - scaledStart : 1;
    };
    span(geometry.x, geometry.width, frameWidth, sequenceWidth, geometry.x,
         geometry.width);
    span(geometry.y, geometry.height, frameHeight, sequenceHeight, geometry.y,
         geometry.height);
    geometry.x = std::min(geometry.x, frameWidth - 1);
    geometry.y = std::min(geometry.y, frameHeight - 1);
    geometry.width = std::min(geometry.width, frameWidth - geometry.x);
    geometry.height = std::min(geometry.height, frameHeight - geometry.y);
    span(geometry.full_x, geometry.full_width, frameWidth, sequenceWidth,
         geometry.full_x, geometry.full_width);
    span(geometry.full_y, geometry.full_height, frameHeight, sequenceHeight,
         geometry.full_y, geometry.full_height);
    geometry.uncovered_left = geometry.x;
    geometry.uncovered_top = geometry.y;
    geometry.uncovered_right = frameWidth - geometry.x - geometry.width;
    geometry.uncovered_bottom = frameHeight - geometry.y - geometry.height;
    geometry.covers_frame =
        geometry.uncovered_left == 0 && geometry.uncovered_top == 0 &&
        geometry.uncovered_right == 0 && geometry.uncovered_bottom == 0;
    geometry.is_identity = geometry.source_x == 0 && geometry.source_y == 0 &&
                           geometry.source_width == sourceWidth &&
                           geometry.source_height == sourceHeight &&
                           geometry.covers_frame;
    return true;
}

void BoundedFrameSize(int64_t frameWidth, int64_t frameHeight, int64_t bound,
                      int64_t& width, int64_t& height) {
    if (frameWidth <= 0 || frameHeight <= 0 || bound <= 0) {
        width = bound > 0 ? bound : 0;
        height = width;
        return;
    }
    if (frameWidth >= frameHeight) {
        width = bound;
        height = (bound * frameHeight) / frameWidth;
        if (height < 2) {
            height = 2;
        }
    } else {
        height = bound;
        width = (bound * frameWidth) / frameHeight;
        if (width < 2) {
            width = 2;
        }
    }
}

bool ClipDisplayedSize(const Document& document, const DocumentClip& clip,
                       int64_t& width, int64_t& height, std::string& error) {
    const LibraryMedia* media = document.FindLibraryMedia(clip.source_id);
    if (media == nullptr) {
        error = "clip '" + clip.id +
                "' names no library media, so its picture has no size to "
                "frame";
        return false;
    }
    if (!media->metadata_complete) {
        error = "media '" + media->id +
                "' was never probed: its size is unknown, and a framing "
                "cannot be resolved without it";
        return false;
    }
    int32_t displayedWidth = 0;
    int32_t displayedHeight = 0;
    if (!DisplayDimensions(*media, displayedWidth, displayedHeight)) {
        error = "media '" + media->id +
                "' has no usable displayed size: it is audio-only, has no "
                "dimensions, or its display rotation is not a right angle";
        return false;
    }
    width = displayedWidth;
    height = displayedHeight;
    error.clear();
    return true;
}

bool ResolveClipFramingForClip(const Document& document,
                               const DocumentClip& clip, int64_t frameWidth,
                               int64_t frameHeight,
                               ClipFramingGeometry& geometry,
                               std::string& error) {
    int64_t width = 0;
    int64_t height = 0;
    if (!ClipDisplayedSize(document, clip, width, height, error)) {
        return false;
    }
    return ResolveClipFraming(clip.framing, width, height, frameWidth,
                              frameHeight, geometry, error);
}

std::string SerializeClipFramingGeometry(const ClipFramingGeometry& geometry) {
    std::ostringstream output;
    output << "{\"source_rect\":{\"x\":" << geometry.source_x
           << ",\"y\":" << geometry.source_y
           << ",\"width\":" << geometry.source_width
           << ",\"height\":" << geometry.source_height
           << "},\"sequence_rect\":{\"x\":" << geometry.x
           << ",\"y\":" << geometry.y << ",\"width\":" << geometry.width
           << ",\"height\":" << geometry.height
           << "},\"picture_rect\":{\"x\":" << geometry.full_x
           << ",\"y\":" << geometry.full_y
           << ",\"width\":" << geometry.full_width
           << ",\"height\":" << geometry.full_height
           << "},\"uncovered\":{\"left\":" << geometry.uncovered_left
           << ",\"top\":" << geometry.uncovered_top
           << ",\"right\":" << geometry.uncovered_right
           << ",\"bottom\":" << geometry.uncovered_bottom
           << "},\"covers_frame\":"
           << (geometry.covers_frame ? "true" : "false") << '}';
    return output.str();
}

std::string ClipFramingCropFilter(const ClipFramingGeometry& geometry,
                                  int64_t sourceWidth, int64_t sourceHeight) {
    if (geometry.source_x == 0 && geometry.source_y == 0 &&
        geometry.source_width == sourceWidth &&
        geometry.source_height == sourceHeight) {
        return std::string();
    }
    std::ostringstream filter;
    filter << "crop=" << geometry.source_width << ':' << geometry.source_height
           << ':' << geometry.source_x << ':' << geometry.source_y;
    return filter.str();
}
