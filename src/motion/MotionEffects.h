#pragma once

// REMOTION-PARITY-2026-09 REM3 -- docs/roadmap/REMOTION-PARITY-2026-09.md,
// docs/MOTION_FORMAT.md. The raster effects of a node -- blur, drop shadow,
// glow, displacement -- on the pixels it painted.
//
// Plain loops on premultiplied float RGBA, no library: the same pixels on
// every machine, which is what lets a rendered effect be compared byte for
// byte. The blur is SVG's feGaussianBlur: from a deviation of 2 pixels,
// three box blurs whose sizes the specification gives; below, the Gaussian
// kernel itself. The painter (MotionRender.mm) draws a node into a layer,
// hands it here and composites the result; the evaluator says which pixels
// it may write (MotionGeometry.h).
//
// Portable: no framework.

#include "MotionComposition.h"
#include "MotionGeometry.h"

#include <cstdint>
#include <vector>

// Premultiplied RGBA in [0, 1], rows from the top.
struct MotionLayer {
    int32_t width = 0;
    int32_t height = 0;
    std::vector<float> rgba;
    // MA7 -- docs/roadmap/MOTION-AFTER-2026-09.md. Where the node's painted
    // area starts, in this layer's pixels: grain() and slice() count their
    // cells and bands from there. The painter cuts the layer at the frame's
    // edge, and a pattern counted from the layer's own corner stayed fixed
    // to the frame while the node slid in past its left or top edge.
    int64_t origin_x = 0;
    int64_t origin_y = 0;
};

// SVG feGaussianBlur of every channel, deviation `sigma` pixels.
void BlurMotionLayer(MotionLayer& layer, double sigma);

// The effects in order. Lengths are the node's own pixels: `transform` is
// the node's transform into the layer's pixels, whose linear part scales
// deviations, amplitudes and wavelengths, and turns a shadow's offset.
void ApplyMotionFilters(MotionLayer& layer,
                        const std::vector<MotionFilter>& filters,
                        const MotionAffine& transform);

// The smooth noise displace() moves pixels by, in [-1, 1]: seeded value
// noise, quintic between lattice points, `channel` 0 or 1 for the two
// directions. Exposed for the tests.
double MotionNoise(double x, double y, double z, int64_t seed, int channel);

// MOTION-DESIGN-2026-09 -- white noise in [-1, 1] of one integer cell, for
// grain: no interpolation between cells.
double MotionNoiseCell(int64_t x, int64_t y, int64_t z, int64_t seed);
