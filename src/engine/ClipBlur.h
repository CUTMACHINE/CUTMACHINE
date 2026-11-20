#pragma once

// EF3 -- docs/roadmap/EFFETS-2026-10.md. A clip's Gaussian blur, as the
// renderers that go through FFmpeg draw it: the export, the sheets and
// read_frame. Written once so that the four cannot spell it four ways.
//
// The blur is a sigma in sequence pixels (DocumentClip::blur). Each picture
// these renderers draw is the clip's picture scaled into a frame that stands
// for the sequence's -- the delivery's resolution, a sheet's cell, a bounded
// read -- so the sigma is carried into that frame's pixels, axis by axis,
// exactly: `blur * frame / sequence`. It is drawn by FFmpeg's `gblur` on the
// picture alone, scaled and cropped but not yet laid in the frame, so that
// its edges are extended and not mixed with the black around it; the monitor
// blurs the same rectangle with the same edges (Renderer.mm).
//
// `gblur` approximates a Gaussian by `steps` passes of a recursive filter
// each way (Alvarez and Mazorra); the monitor draws a true Gaussian with
// Metal Performance Shaders. kClipBlurGblurSteps is the count that brings
// the two closest, measured by tests/monitor_export_parity_tests.mm, where
// the remaining gap is bounded.

#include "Document.h"

#include <cstdint>
#include <numeric>
#include <sstream>
#include <string>

// FFmpeg's `gblur` takes 1 to 6 steps. Six, the most, is the closest to a
// Gaussian, and the parity test measured it the closest to the monitor's.
inline constexpr int kClipBlurGblurSteps = 6;

// FFmpeg's `gblur` refuses a sigma past 1024 of its pixels.
inline constexpr int64_t kGblurMaxSigma = 1024;

// The blur's sigma in the pixels of a frame `frameSize` wide (or high) that
// stands for a sequence `sequenceSize` wide (or high), reduced. False when a
// size is not positive or the fraction overflows.
inline bool ClipBlurSigmaInFrame(const EffectParamValue& blur,
                                 int64_t frameSize, int64_t sequenceSize,
                                 int64_t& num, int64_t& den) {
    if (blur.den <= 0 || blur.num < 0 || frameSize <= 0 || sequenceSize <= 0)
        return false;
    num = static_cast<int64_t>(blur.num) * frameSize;
    den = static_cast<int64_t>(blur.den) * sequenceSize;
    const int64_t common = std::gcd(num, den);
    if (common > 1) {
        num /= common;
        den /= common;
    }
    return true;
}

// `gblur=...` drawing `clip`'s blur on a picture laid in a frame of
// `frameWidth` x `frameHeight` that stands for the sequence's
// `sequenceWidth` x `sequenceHeight`; empty when the clip has no blur. The
// sigmas are spelled as the exact fractions they are, `(num/den)`, which
// FFmpeg evaluates, as the opacity's `aa=(num/den)` is. False, with
// `error`, when the frame gives a sigma `gblur` cannot take.
inline bool ClipBlurFilter(const DocumentClip& clip, int64_t frameWidth,
                           int64_t frameHeight, int64_t sequenceWidth,
                           int64_t sequenceHeight, std::string& filter,
                           std::string& error) {
    filter.clear();
    if (clip.blur.num == 0) return true;
    int64_t horizontalNum = 0;
    int64_t horizontalDen = 1;
    int64_t verticalNum = 0;
    int64_t verticalDen = 1;
    if (!ClipBlurSigmaInFrame(clip.blur, frameWidth, sequenceWidth,
                              horizontalNum, horizontalDen) ||
        !ClipBlurSigmaInFrame(clip.blur, frameHeight, sequenceHeight,
                              verticalNum, verticalDen)) {
        error = "clip '" + clip.id +
                "' carries a blur, and the sequence or the picture it is "
                "drawn in has no size to measure it in";
        return false;
    }
    if (horizontalNum > kGblurMaxSigma * horizontalDen ||
        verticalNum > kGblurMaxSigma * verticalDen) {
        error = "clip '" + clip.id +
                "': its blur, carried to this resolution, is past the " +
                std::to_string(kGblurMaxSigma) +
                " pixels FFmpeg's gblur takes; export at a resolution "
                "nearer the sequence's, or lower the blur";
        return false;
    }
    std::ostringstream spelled;
    spelled.imbue(std::locale::classic());
    spelled << "gblur=sigma=(" << horizontalNum << '/' << horizontalDen
            << "):steps=" << kClipBlurGblurSteps;
    // A frame of the sequence's own shape scales both axes alike, and the
    // vertical sigma is then left to its default, the horizontal one.
    if (horizontalNum != verticalNum || horizontalDen != verticalDen) {
        spelled << ":sigmaV=(" << verticalNum << '/' << verticalDen << ')';
    }
    filter = spelled.str();
    return true;
}
