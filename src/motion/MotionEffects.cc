#include "MotionEffects.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

constexpr double kPi = 3.14159265358979323846;

// MA7 -- docs/roadmap/MOTION-AFTER-2026-09.md. A noise coordinate is any
// finite number: a phase is written so, and an animation that extends past
// its keys carries it anywhere. Past 2^53 a double has no fraction left,
// past 2^63 its whole part has no int64 and the cast is undefined: it is
// held at 2^53.
constexpr double kFarthestCoordinate = 9007199254740992.0;  // 2^53

double Held(double coordinate) {
    return std::clamp(coordinate, -kFarthestCoordinate, kFarthestCoordinate);
}

// MA7 -- the whole part of the phase of grain() or slice(). A phase mixed
// in floating point lands a rounding short of the whole number it should
// be: from 0 to 22 over 22 frames, frame 15 is 14.999999999999998, and its
// floor showed frame 14's pattern again (3.7 % of the frames of such
// animations 100 to 1000 frames long). A phase within a few ulps of a whole
// number is that number.
int64_t WholePhase(double phase) {
    const double held = Held(phase);
    const double nearest = std::round(held);
    const double ulps = 8.0 * std::numeric_limits<double>::epsilon() *
                        std::max(1.0, std::abs(nearest));
    return static_cast<int64_t>(
        std::abs(held - nearest) <= ulps ? nearest : std::floor(held));
}

// One box blur along rows or columns: out[i] is the mean of in[i - behind]
// .. in[i + ahead], transparent past the edges.
void BoxPass(std::vector<float>& data, int32_t width, int32_t height,
             bool horizontal, int behind, int ahead) {
    const int32_t lines = horizontal ? height : width;
    const int32_t length = horizontal ? width : height;
    const size_t stride = horizontal ? 4 : static_cast<size_t>(width) * 4;
    const float scale = 1.0f / static_cast<float>(behind + ahead + 1);
    std::vector<float> line(static_cast<size_t>(length) * 4);
    for (int32_t index = 0; index < lines; ++index) {
        const size_t start = horizontal ? static_cast<size_t>(index) * width * 4
                                        : static_cast<size_t>(index) * 4;
        for (int32_t at = 0; at < length; ++at) {
            for (int channel = 0; channel < 4; ++channel) {
                line[static_cast<size_t>(at) * 4 + channel] =
                    data[start + static_cast<size_t>(at) * stride + channel];
            }
        }
        for (int channel = 0; channel < 4; ++channel) {
            double sum = 0.0;
            // The window of output 0: [-behind, ahead].
            for (int32_t at = 0; at <= ahead && at < length; ++at) {
                sum += line[static_cast<size_t>(at) * 4 + channel];
            }
            for (int32_t at = 0; at < length; ++at) {
                data[start + static_cast<size_t>(at) * stride + channel] =
                    static_cast<float>(sum) * scale;
                const int32_t entering = at + ahead + 1;
                const int32_t leaving = at - behind;
                if (entering < length) {
                    sum += line[static_cast<size_t>(entering) * 4 + channel];
                }
                if (leaving >= 0) {
                    sum -= line[static_cast<size_t>(leaving) * 4 + channel];
                }
            }
        }
    }
}

// A true Gaussian along rows or columns, for small deviations.
void GaussianPass(std::vector<float>& data, int32_t width, int32_t height,
                  bool horizontal, double sigma) {
    const int radius = static_cast<int>(std::ceil(3.0 * sigma));
    std::vector<double> kernel(static_cast<size_t>(2 * radius + 1));
    double total = 0.0;
    for (int k = -radius; k <= radius; ++k) {
        const double weight = std::exp(-(k * k) / (2.0 * sigma * sigma));
        const int tap = k + radius;
        kernel[static_cast<size_t>(tap)] = weight;
        total += weight;
    }
    for (double& weight : kernel) {
        weight /= total;
    }
    const int32_t lines = horizontal ? height : width;
    const int32_t length = horizontal ? width : height;
    const size_t stride = horizontal ? 4 : static_cast<size_t>(width) * 4;
    std::vector<float> line(static_cast<size_t>(length) * 4);
    for (int32_t index = 0; index < lines; ++index) {
        const size_t start = horizontal ? static_cast<size_t>(index) * width * 4
                                        : static_cast<size_t>(index) * 4;
        for (int32_t at = 0; at < length; ++at) {
            for (int channel = 0; channel < 4; ++channel) {
                line[static_cast<size_t>(at) * 4 + channel] =
                    data[start + static_cast<size_t>(at) * stride + channel];
            }
        }
        for (int32_t at = 0; at < length; ++at) {
            for (int channel = 0; channel < 4; ++channel) {
                double sum = 0.0;
                for (int k = -radius; k <= radius; ++k) {
                    const int32_t source = at + k;
                    if (source >= 0 && source < length) {
                        const int tap = k + radius;
                        sum += kernel[static_cast<size_t>(tap)] *
                               line[static_cast<size_t>(source) * 4 + channel];
                    }
                }
                data[start + static_cast<size_t>(at) * stride + channel] =
                    static_cast<float>(sum);
            }
        }
    }
}

void BlurAxis(std::vector<float>& data, int32_t width, int32_t height,
              bool horizontal, double sigma) {
    if (sigma <= 0.0) {
        return;
    }
    if (sigma < 2.0) {
        GaussianPass(data, width, height, horizontal, sigma);
        return;
    }
    // SVG 1.1, feGaussianBlur: d = floor(s * 3 * sqrt(2 * pi) / 4 + 0.5).
    const int d = static_cast<int>(
        std::floor(sigma * 3.0 * std::sqrt(2.0 * kPi) / 4.0 + 0.5));
    if (d % 2 == 1) {
        for (int pass = 0; pass < 3; ++pass) {
            BoxPass(data, width, height, horizontal, d / 2, d / 2);
        }
        return;
    }
    // Even: two boxes of d, centred on the boundary to the left then to the
    // right of the pixel, and one of d + 1 centred on it.
    BoxPass(data, width, height, horizontal, d / 2, d / 2 - 1);
    BoxPass(data, width, height, horizontal, d / 2 - 1, d / 2);
    BoxPass(data, width, height, horizontal, d / 2, d / 2);
}

// The layer's coverage, blurred: the shape of a shadow or a glow.
std::vector<float> BlurredAlpha(const MotionLayer& layer, double sigma) {
    MotionLayer alpha;
    alpha.width = layer.width;
    alpha.height = layer.height;
    alpha.rgba.assign(layer.rgba.size(), 0.0f);
    for (size_t at = 3; at < layer.rgba.size(); at += 4) {
        alpha.rgba[at] = layer.rgba[at];
    }
    BlurMotionLayer(alpha, sigma);
    std::vector<float> out(layer.rgba.size() / 4);
    for (size_t at = 0; at < out.size(); ++at) {
        out[at] = alpha.rgba[at * 4 + 3];
    }
    return out;
}

// Bilinear sample of a premultiplied layer, transparent outside.
void Sample(const MotionLayer& layer, double x, double y, float out[4]) {
    const double fx = std::floor(x);
    const double fy = std::floor(y);
    const double tx = x - fx;
    const double ty = y - fy;
    for (int channel = 0; channel < 4; ++channel) {
        out[channel] = 0.0f;
    }
    for (int dy = 0; dy <= 1; ++dy) {
        for (int dx = 0; dx <= 1; ++dx) {
            const int64_t px = static_cast<int64_t>(fx) + dx;
            const int64_t py = static_cast<int64_t>(fy) + dy;
            if (px < 0 || py < 0 || px >= layer.width || py >= layer.height) {
                continue;
            }
            const double weight =
                ((dx != 0) ? tx : 1.0 - tx) * ((dy != 0) ? ty : 1.0 - ty);
            const size_t at = (static_cast<size_t>(py) * layer.width +
                               static_cast<size_t>(px)) *
                              4;
            for (int channel = 0; channel < 4; ++channel) {
                out[channel] +=
                    static_cast<float>(weight * layer.rgba[at + channel]);
            }
        }
    }
}

// `under` (premultiplied colour times coverage) beneath the layer: the
// layer over it, source-over.
void UnderLayer(MotionLayer& layer, const std::vector<float>& coverage,
                const MotionColor& color, double shiftX, double shiftY) {
    const MotionLayer source = layer;
    const float red = static_cast<float>(color.red * color.alpha);
    const float green = static_cast<float>(color.green * color.alpha);
    const float blue = static_cast<float>(color.blue * color.alpha);
    const float alpha = static_cast<float>(color.alpha);
    MotionLayer shape;
    shape.width = layer.width;
    shape.height = layer.height;
    shape.rgba.assign(layer.rgba.size(), 0.0f);
    for (size_t at = 0; at < coverage.size(); ++at) {
        shape.rgba[at * 4 + 3] = coverage[at];
    }
    for (int32_t y = 0; y < layer.height; ++y) {
        for (int32_t x = 0; x < layer.width; ++x) {
            float shifted[4];
            Sample(shape, x - shiftX, y - shiftY, shifted);
            const float cover = shifted[3];
            const size_t at = (static_cast<size_t>(y) * layer.width + x) * 4;
            const float keep = 1.0f - source.rgba[at + 3];
            layer.rgba[at] = source.rgba[at] + red * cover * keep;
            layer.rgba[at + 1] = source.rgba[at + 1] + green * cover * keep;
            layer.rgba[at + 2] = source.rgba[at + 2] + blue * cover * keep;
            layer.rgba[at + 3] = source.rgba[at + 3] + alpha * cover * keep;
        }
    }
}

uint64_t Mix(uint64_t value) {
    value += 0x9E3779B97F4A7C15ULL;
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31);
}

double Lattice(int64_t x, int64_t y, int64_t z, int64_t seed, int channel) {
    uint64_t hash =
        Mix(static_cast<uint64_t>(seed) * 4 + static_cast<uint64_t>(channel));
    hash = Mix(hash ^ static_cast<uint64_t>(x));
    hash = Mix(hash ^ static_cast<uint64_t>(y));
    hash = Mix(hash ^ static_cast<uint64_t>(z));
    return static_cast<double>(hash >> 11) / static_cast<double>(1ULL << 53) *
               2.0 -
           1.0;
}

double Fade(double t) { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); }

}  // namespace

double MotionNoiseCell(int64_t x, int64_t y, int64_t z, int64_t seed) {
    return Lattice(x, y, z, seed, 2);
}

namespace {

// MOTION-DESIGN-2026-09 -- slice(): the shift of row `y`, -1 to 1. Each band
// splits once between a fifth and four fifths of its height, and two slices
// in five stay in place.
double SliceShift(int64_t y, double band, int64_t step, int64_t seed) {
    const double position = y / band;
    const auto cell = static_cast<int64_t>(std::floor(position));
    const double split = 0.5 + 0.3 * MotionNoiseCell(cell, 1, step, seed);
    const int64_t part = cell * 2 + (position - cell >= split ? 1 : 0);
    if (std::abs(MotionNoiseCell(part, 2, step, seed)) < 0.4) {
        return 0.0;
    }
    return MotionNoiseCell(part, 3, step, seed);
}

// MOTION-DESIGN-2026-09 -- mesh(), After Effects' Mesh Warp.
struct MeshPoint {
    double x = 0.0;
    double y = 0.0;
};

MeshPoint MeshBezier(const MeshPoint& p0, const MeshPoint& p1,
                     const MeshPoint& p2, const MeshPoint& p3, double t) {
    const double s = 1.0 - t;
    const double a = s * s * s;
    const double b = 3.0 * s * s * t;
    const double c = 3.0 * s * t * t;
    const double d = t * t * t;
    return {a * p0.x + b * p1.x + c * p2.x + d * p3.x,
            a * p0.y + b * p1.y + c * p2.y + d * p3.y};
}

// Premultiplied bilinear read of `layer` at a point of its pixels (pixel
// centres at half units); outside it, transparent.
void SampleLayer(const MotionLayer& layer, double x, double y, float out[4]) {
    const double px = x - 0.5;
    const double py = y - 0.5;
    const double fx = std::floor(px);
    const double fy = std::floor(py);
    const auto x0 = static_cast<int64_t>(fx);
    const auto y0 = static_cast<int64_t>(fy);
    const double tx = px - fx;
    const double ty = py - fy;
    const double weights[4] = {(1.0 - tx) * (1.0 - ty), tx * (1.0 - ty),
                               (1.0 - tx) * ty, tx * ty};
    double sum[4] = {};
    for (int tap = 0; tap < 4; ++tap) {
        const int64_t sx = x0 + (tap & 1);
        const int64_t sy = y0 + (tap >> 1);
        if (sx < 0 || sy < 0 || sx >= layer.width || sy >= layer.height) {
            continue;
        }
        const size_t at =
            (static_cast<size_t>(sy) * static_cast<size_t>(layer.width) +
             static_cast<size_t>(sx)) *
            4;
        for (int channel = 0; channel < 4; ++channel) {
            sum[channel] += weights[tap] * layer.rgba[at + channel];
        }
    }
    for (int channel = 0; channel < 4; ++channel) {
        out[channel] = static_cast<float>(sum[channel]);
    }
}

// One triangle of the tessellated mesh: every pixel whose centre it covers
// reads the source at the matching point of the source triangle. Shared
// edges are written twice with the same value, so the order does not show.
void WarpTriangle(const MotionLayer& source, MotionLayer& target,
                  const MeshPoint to[3], const MeshPoint from[3]) {
    const double area = (to[1].x - to[0].x) * (to[2].y - to[0].y) -
                        (to[2].x - to[0].x) * (to[1].y - to[0].y);
    if (std::abs(area) < 1e-12) {
        return;
    }
    const double left = std::min({to[0].x, to[1].x, to[2].x});
    const double right = std::max({to[0].x, to[1].x, to[2].x});
    const double top = std::min({to[0].y, to[1].y, to[2].y});
    const double bottom = std::max({to[0].y, to[1].y, to[2].y});
    const auto x0 = static_cast<int32_t>(std::max(0.0, std::floor(left - 0.5)));
    const auto x1 = static_cast<int32_t>(
        std::min<double>(target.width - 1, std::ceil(right - 0.5)));
    const auto y0 = static_cast<int32_t>(std::max(0.0, std::floor(top - 0.5)));
    const auto y1 = static_cast<int32_t>(
        std::min<double>(target.height - 1, std::ceil(bottom - 0.5)));
    const double tolerance = 1e-9;
    for (int32_t y = y0; y <= y1; ++y) {
        const double cy = y + 0.5;
        for (int32_t x = x0; x <= x1; ++x) {
            const double cx = x + 0.5;
            const double w0 = ((to[1].x - cx) * (to[2].y - cy) -
                               (to[2].x - cx) * (to[1].y - cy)) /
                              area;
            const double w1 = ((to[2].x - cx) * (to[0].y - cy) -
                               (to[0].x - cx) * (to[2].y - cy)) /
                              area;
            const double w2 = 1.0 - w0 - w1;
            if (w0 < -tolerance || w1 < -tolerance || w2 < -tolerance) {
                continue;
            }
            float texel[4];
            SampleLayer(
                source, w0 * from[0].x + w1 * from[1].x + w2 * from[2].x,
                w0 * from[0].y + w1 * from[1].y + w2 * from[2].y, texel);
            const size_t at =
                (static_cast<size_t>(y) * static_cast<size_t>(target.width) +
                 static_cast<size_t>(x)) *
                4;
            for (int channel = 0; channel < 4; ++channel) {
                target.rgba[at + channel] = texel[channel];
            }
        }
    }
}

// The node's box is a grid of Coons patches. Each cell is bounded by four
// cubic Béziers joining its corners, with handles a third of the resting
// cell away from each vertex, moving with it (After's default, what the
// PANTER kinetic warp relies on). Each cell is cut into small quads, fine
// enough that the flat pieces follow the curves to a fraction of a pixel,
// and each quad carries the matching part of the resting cell. After warps
// the whole layer, so nothing a mesh moves may stay behind: past the resting
// box, the picture follows the nearest point of its edge (an accent above a
// tight line stays on its letter, an edge pulled in takes its antialiasing
// along), and the cells are drawn last, so they win where the two fold.
void WarpMesh(MotionLayer& layer, const MotionFilter& filter,
              const MotionAffine& transform) {
    const int32_t rows = filter.mesh_rows;
    const int32_t columns = filter.mesh_columns;
    if (rows < 1 || columns < 1 ||
        filter.mesh_resolved.size() !=
            static_cast<size_t>(rows + 1) * static_cast<size_t>(columns + 1) ||
        filter.mesh_box_width <= 0.0 || filter.mesh_box_height <= 0.0) {
        return;
    }
    const MotionLayer source = layer;
    std::fill(layer.rgba.begin(), layer.rgba.end(), 0.0f);
    const MotionAffine inverse = transform.Inverse();
    const double boxLeft = filter.mesh_box_x;
    const double boxTop = filter.mesh_box_y;
    const double boxRight = boxLeft + filter.mesh_box_width;
    const double boxBottom = boxTop + filter.mesh_box_height;
    // How far the layer reaches past the resting box, in node units: the
    // distance is convex, so the layer's corners bound it.
    double margin = 0.0;
    for (const double x : {0.0, static_cast<double>(layer.width)}) {
        for (const double y : {0.0, static_cast<double>(layer.height)}) {
            double nx = 0.0;
            double ny = 0.0;
            inverse.Apply(x, y, nx, ny);
            margin = std::max({margin, boxLeft - nx, nx - boxRight, boxTop - ny,
                               ny - boxBottom});
        }
    }
    margin += 1.0;
    const double cellWidth = filter.mesh_box_width / columns;
    const double cellHeight = filter.mesh_box_height / rows;
    const double handleX = cellWidth / 3.0;
    const double handleY = cellHeight / 3.0;
    const auto vertex = [&](int32_t row, int32_t column) {
        const auto& point =
            filter.mesh_resolved[static_cast<size_t>(row) * (columns + 1) +
                                 static_cast<size_t>(column)];
        return MeshPoint{point.first, point.second};
    };
    const auto toLayer = [&](const MeshPoint& point) {
        MeshPoint out;
        transform.Apply(point.x, point.y, out.x, out.y);
        return out;
    };
    const auto shifted = [](const MeshPoint& p, double dx, double dy) {
        return MeshPoint{p.x + dx, p.y + dy};
    };
    // A quad of node points, warped `to` from its resting `from`.
    const auto quad = [&](const MeshPoint to[4], const MeshPoint from[4]) {
        const MeshPoint firstTo[3] = {toLayer(to[0]), toLayer(to[1]),
                                      toLayer(to[2])};
        const MeshPoint firstFrom[3] = {toLayer(from[0]), toLayer(from[1]),
                                        toLayer(from[2])};
        const MeshPoint secondTo[3] = {toLayer(to[0]), toLayer(to[2]),
                                       toLayer(to[3])};
        const MeshPoint secondFrom[3] = {toLayer(from[0]), toLayer(from[2]),
                                         toLayer(from[3])};
        WarpTriangle(source, layer, firstTo, firstFrom);
        WarpTriangle(source, layer, secondTo, secondFrom);
    };
    // A strip from a piece of the resting edge out to the margin, moved as
    // that piece of the edge moved.
    const auto strip = [&](const MeshPoint& restA, const MeshPoint& restB,
                           const MeshPoint& warpA, const MeshPoint& warpB,
                           double dx, double dy) {
        const MeshPoint to[4] = {warpA, warpB, shifted(warpB, dx, dy),
                                 shifted(warpA, dx, dy)};
        const MeshPoint from[4] = {restA, restB, shifted(restB, dx, dy),
                                   shifted(restA, dx, dy)};
        quad(to, from);
    };
    // The quadrant past a corner of the box, moved as its vertex moved.
    const auto corner = [&](const MeshPoint& rest, const MeshPoint& moved,
                            double dx, double dy) {
        const MeshPoint from[4] = {rest, shifted(rest, dx, 0.0),
                                   shifted(rest, dx, dy),
                                   shifted(rest, 0.0, dy)};
        const double mx = moved.x - rest.x;
        const double my = moved.y - rest.y;
        const MeshPoint to[4] = {
            shifted(from[0], mx, my), shifted(from[1], mx, my),
            shifted(from[2], mx, my), shifted(from[3], mx, my)};
        quad(to, from);
    };
    corner({boxLeft, boxTop}, vertex(0, 0), -margin, -margin);
    corner({boxRight, boxTop}, vertex(0, columns), margin, -margin);
    corner({boxLeft, boxBottom}, vertex(rows, 0), -margin, margin);
    corner({boxRight, boxBottom}, vertex(rows, columns), margin, margin);
    // First pass: the strips past the edge cells; second: the cells.
    for (int pass = 0; pass < 2; ++pass) {
        for (int32_t row = 0; row < rows; ++row) {
            for (int32_t column = 0; column < columns; ++column) {
                const bool border = row == 0 || row == rows - 1 ||
                                    column == 0 || column == columns - 1;
                if (pass == 0 && !border) {
                    continue;
                }
                const MeshPoint p00 = vertex(row, column);
                const MeshPoint p10 = vertex(row, column + 1);
                const MeshPoint p01 = vertex(row + 1, column);
                const MeshPoint p11 = vertex(row + 1, column + 1);
                const auto surface = [&](double u, double v) {
                    const MeshPoint top =
                        MeshBezier(p00, shifted(p00, handleX, 0.0),
                                   shifted(p10, -handleX, 0.0), p10, u);
                    const MeshPoint bottom =
                        MeshBezier(p01, shifted(p01, handleX, 0.0),
                                   shifted(p11, -handleX, 0.0), p11, u);
                    const MeshPoint left =
                        MeshBezier(p00, shifted(p00, 0.0, handleY),
                                   shifted(p01, 0.0, -handleY), p01, v);
                    const MeshPoint right =
                        MeshBezier(p10, shifted(p10, 0.0, handleY),
                                   shifted(p11, 0.0, -handleY), p11, v);
                    const double a = (1.0 - u) * (1.0 - v);
                    const double b = u * (1.0 - v);
                    const double c = (1.0 - u) * v;
                    const double d = u * v;
                    return MeshPoint{
                        (1.0 - v) * top.x + v * bottom.x + (1.0 - u) * left.x +
                            u * right.x -
                            (a * p00.x + b * p10.x + c * p01.x + d * p11.x),
                        (1.0 - v) * top.y + v * bottom.y + (1.0 - u) * left.y +
                            u * right.y -
                            (a * p00.y + b * p10.y + c * p01.y + d * p11.y)};
                };
                // As many pieces as the cell spans three layer pixels, at
                // least twice the quality, at most 512 a side.
                double extent = 0.0;
                const MeshPoint corners[4] = {toLayer(p00), toLayer(p10),
                                              toLayer(p01), toLayer(p11)};
                for (const MeshPoint& a : corners) {
                    for (const MeshPoint& b : corners) {
                        extent = std::max(
                            {extent, std::abs(a.x - b.x), std::abs(a.y - b.y)});
                    }
                }
                extent +=
                    2.0 * transform.LengthScale() * std::max(handleX, handleY);
                const int32_t pieces = static_cast<int32_t>(
                    std::clamp(std::max<double>(2.0 * filter.mesh_quality,
                                                std::ceil(extent / 3.0)),
                               2.0, 512.0));
                const double restLeft = boxLeft + cellWidth * column;
                const double restTop = boxTop + cellHeight * row;
                const auto rest = [&](double u, double v) {
                    return MeshPoint{restLeft + u * cellWidth,
                                     restTop + v * cellHeight};
                };
                if (pass == 0) {
                    // The strips meet the cells on points computed the same
                    // way, so no pixel falls between them.
                    for (int32_t i = 0; i < pieces; ++i) {
                        const double s0 = static_cast<double>(i) / pieces;
                        const double s1 = static_cast<double>(i + 1) / pieces;
                        if (row == 0) {
                            strip(rest(s0, 0.0), rest(s1, 0.0),
                                  surface(s0, 0.0), surface(s1, 0.0), 0.0,
                                  -margin);
                        }
                        if (row == rows - 1) {
                            strip(rest(s0, 1.0), rest(s1, 1.0),
                                  surface(s0, 1.0), surface(s1, 1.0), 0.0,
                                  margin);
                        }
                        if (column == 0) {
                            strip(rest(0.0, s0), rest(0.0, s1),
                                  surface(0.0, s0), surface(0.0, s1), -margin,
                                  0.0);
                        }
                        if (column == columns - 1) {
                            strip(rest(1.0, s0), rest(1.0, s1),
                                  surface(1.0, s0), surface(1.0, s1), margin,
                                  0.0);
                        }
                    }
                    continue;
                }
                std::vector<MeshPoint> to(static_cast<size_t>(pieces + 1) *
                                          (pieces + 1));
                std::vector<MeshPoint> from(to.size());
                for (int32_t j = 0; j <= pieces; ++j) {
                    const double v = static_cast<double>(j) / pieces;
                    for (int32_t i = 0; i <= pieces; ++i) {
                        const double u = static_cast<double>(i) / pieces;
                        const size_t at =
                            static_cast<size_t>(j) * (pieces + 1) + i;
                        to[at] = surface(u, v);
                        from[at] = rest(u, v);
                    }
                }
                for (int32_t j = 0; j < pieces; ++j) {
                    for (int32_t i = 0; i < pieces; ++i) {
                        const size_t a =
                            static_cast<size_t>(j) * (pieces + 1) + i;
                        const size_t c = a + (pieces + 1);
                        const MeshPoint cellTo[4] = {to[a], to[a + 1],
                                                     to[c + 1], to[c]};
                        const MeshPoint cellFrom[4] = {from[a], from[a + 1],
                                                       from[c + 1], from[c]};
                        quad(cellTo, cellFrom);
                    }
                }
            }
        }
    }
}

}  // namespace

double MotionNoise(double x, double y, double z, int64_t seed, int channel) {
    x = Held(x);
    y = Held(y);
    z = Held(z);
    const double fx = std::floor(x);
    const double fy = std::floor(y);
    const double fz = std::floor(z);
    const auto ix = static_cast<int64_t>(fx);
    const auto iy = static_cast<int64_t>(fy);
    const auto iz = static_cast<int64_t>(fz);
    const double u = Fade(x - fx);
    const double v = Fade(y - fy);
    const double w = Fade(z - fz);
    double layers[2];
    for (int dz = 0; dz <= 1; ++dz) {
        const double a = Lattice(ix, iy, iz + dz, seed, channel);
        const double b = Lattice(ix + 1, iy, iz + dz, seed, channel);
        const double c = Lattice(ix, iy + 1, iz + dz, seed, channel);
        const double d = Lattice(ix + 1, iy + 1, iz + dz, seed, channel);
        const double top = a + (b - a) * u;
        const double bottom = c + (d - c) * u;
        layers[dz] = top + (bottom - top) * v;
    }
    return layers[0] + (layers[1] - layers[0]) * w;
}

void BlurMotionLayer(MotionLayer& layer, double sigma) {
    BlurAxis(layer.rgba, layer.width, layer.height, true, sigma);
    BlurAxis(layer.rgba, layer.width, layer.height, false, sigma);
}

void ApplyMotionFilters(MotionLayer& layer,
                        const std::vector<MotionFilter>& filters,
                        const MotionAffine& transform) {
    const double scale = transform.LengthScale();
    for (const MotionFilter& filter : filters) {
        switch (filter.kind) {
            case MotionFilterKind::Blur:
                BlurMotionLayer(layer, filter.sigma * scale);
                break;
            case MotionFilterKind::DropShadow: {
                const std::vector<float> coverage =
                    BlurredAlpha(layer, filter.sigma * scale);
                // The offset turns and scales with the node.
                const double shiftX = transform.a * filter.offset_x +
                                      transform.c * filter.offset_y;
                const double shiftY = transform.b * filter.offset_x +
                                      transform.d * filter.offset_y;
                UnderLayer(layer, coverage, filter.color, shiftX, shiftY);
                break;
            }
            case MotionFilterKind::Glow: {
                std::vector<float> coverage =
                    BlurredAlpha(layer, filter.sigma * scale);
                // Bounded: a strong glow saturates at full coverage, it
                // never spreads past its blur.
                for (float& cover : coverage) {
                    cover = std::min(
                        1.0f, cover * static_cast<float>(filter.strength));
                }
                UnderLayer(layer, coverage, filter.color, 0.0, 0.0);
                break;
            }
            case MotionFilterKind::Goo: {
                // MOTION-DESIGN-2026-09 -- blur every channel, then a steep
                // ramp of coverage around one half; colour kept straight.
                BlurMotionLayer(layer, filter.sigma * scale);
                for (size_t at = 0; at + 3 < layer.rgba.size(); at += 4) {
                    const float alpha = layer.rgba[at + 3];
                    const float edge =
                        std::clamp((alpha - 0.46f) / 0.08f, 0.0f, 1.0f);
                    const float hard = edge * edge * (3.0f - 2.0f * edge);
                    const float gain = alpha > 1e-4f ? hard / alpha : 0.0f;
                    for (int channel = 0; channel < 3; ++channel) {
                        layer.rgba[at + channel] =
                            std::min(hard, layer.rgba[at + channel] * gain);
                    }
                    layer.rgba[at + 3] = hard;
                }
                break;
            }
            case MotionFilterKind::Grain: {
                // MOTION-DESIGN-2026-09 -- one value per cell, the same on
                // the three channels, scaled by coverage so that the layer
                // stays premultiplied.
                const double cell = std::max(1.0, filter.wavelength * scale);
                const int64_t grain = WholePhase(filter.phase);
                for (int32_t y = 0; y < layer.height; ++y) {
                    for (int32_t x = 0; x < layer.width; ++x) {
                        const size_t at =
                            (static_cast<size_t>(y) * layer.width + x) * 4;
                        const float alpha = layer.rgba[at + 3];
                        if (alpha <= 0.0f) {
                            continue;
                        }
                        const double noise =
                            filter.amplitude *
                            MotionNoiseCell(static_cast<int64_t>(std::floor(
                                                (x - layer.origin_x) / cell)),
                                            static_cast<int64_t>(std::floor(
                                                (y - layer.origin_y) / cell)),
                                            grain, filter.seed);
                        for (int channel = 0; channel < 3; ++channel) {
                            layer.rgba[at + channel] = std::clamp(
                                layer.rgba[at + channel] +
                                    static_cast<float>(noise) * alpha,
                                0.0f, alpha);
                        }
                    }
                }
                break;
            }
            case MotionFilterKind::Mesh:
                WarpMesh(layer, filter, transform);
                break;
            case MotionFilterKind::Slice: {
                // MOTION-DESIGN-2026-09 -- rows move whole, by whole pixels.
                const MotionLayer source = layer;
                const double amplitude = filter.amplitude * scale;
                const double band = std::max(1.0, filter.wavelength * scale);
                const int64_t step = WholePhase(filter.phase);
                for (int32_t y = 0; y < layer.height; ++y) {
                    const auto shift = static_cast<int32_t>(std::lround(
                        amplitude * SliceShift(y - layer.origin_y, band, step,
                                               filter.seed)));
                    const size_t row = static_cast<size_t>(y) * layer.width;
                    for (int32_t x = 0; x < layer.width; ++x) {
                        const int32_t from = x - shift;
                        const size_t at = (row + x) * 4;
                        for (int channel = 0; channel < 4; ++channel) {
                            layer.rgba[at + channel] =
                                from < 0 || from >= layer.width
                                    ? 0.0f
                                    : source.rgba[(row + from) * 4 + channel];
                        }
                    }
                }
                break;
            }
            case MotionFilterKind::Displace: {
                const MotionLayer source = layer;
                const double amplitude = filter.amplitude * scale;
                const double wavelength = filter.wavelength * scale;
                for (int32_t y = 0; y < layer.height; ++y) {
                    for (int32_t x = 0; x < layer.width; ++x) {
                        const double nx = x / wavelength;
                        const double ny = y / wavelength;
                        const double offsetX =
                            amplitude *
                            MotionNoise(nx, ny, filter.phase, filter.seed, 0);
                        const double offsetY =
                            amplitude *
                            MotionNoise(nx, ny, filter.phase, filter.seed, 1);
                        float sampled[4];
                        Sample(source, x + offsetX, y + offsetY, sampled);
                        const size_t at =
                            (static_cast<size_t>(y) * layer.width + x) * 4;
                        for (int channel = 0; channel < 4; ++channel) {
                            layer.rgba[at + channel] = sampled[channel];
                        }
                    }
                }
                break;
            }
        }
    }
}
