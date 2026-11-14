#include "MotionGeometry.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

constexpr double kPi = 3.14159265358979323846;

MotionAffine Translation(double x, double y) {
    MotionAffine affine;
    affine.tx = x;
    affine.ty = y;
    return affine;
}

struct Extent {
    double min_x = std::numeric_limits<double>::infinity();
    double min_y = std::numeric_limits<double>::infinity();
    double max_x = -std::numeric_limits<double>::infinity();
    double max_y = -std::numeric_limits<double>::infinity();

    void Add(double x, double y) {
        min_x = std::min(min_x, x);
        min_y = std::min(min_y, y);
        max_x = std::max(max_x, x);
        max_y = std::max(max_y, y);
    }

    // A projective map keeps straight lines straight: a box's corners bound
    // its image, unless one of them falls behind the viewer, in which case
    // that corner is left out.
    void AddBox(const MotionBox& box, const MotionHomography& transform,
                double inflate) {
        const double xs[] = {box.x - inflate, box.x + box.width + inflate};
        const double ys[] = {box.y - inflate, box.y + box.height + inflate};
        for (const double x : xs) {
            for (const double y : ys) {
                double outX = 0.0;
                double outY = 0.0;
                if (transform.Apply(x, y, outX, outY)) {
                    Add(outX, outY);
                }
            }
        }
    }

    bool Empty() const { return min_x > max_x; }
};

// What a node paints past its box: antialiasing everywhere, a stroke's
// outer half, a glyph's overhang (an italic's, most of all).
double Overhang(const MotionEvaluatedNode& node) {
    double inflate = 2.0;
    if (node.type == MotionNodeType::Path) {
        inflate += node.path_paint.stroke_width / 2.0;
    }
    if (node.type == MotionNodeType::Text) {
        inflate += node.text_style.stroke_width / 2.0 +
                   0.15 * node.text_style.font_size;
    }
    return inflate;
}

void Collect(const MotionEvaluatedNode& node, const MotionHomography& parent,
             Extent& extent) {
    const MotionHomography transform = parent.After(MotionNodeMapping(node));
    extent.AddBox(node.box, transform, Overhang(node));
    for (const MotionEvaluatedNode& child : node.children) {
        Collect(child, transform, extent);
    }
    if (!node.instance_root.empty()) {
        Collect(node.instance_root.front(),
                transform.After(MotionHomography::FromAffine(
                    MotionInstanceTransform(node))),
                extent);
    }
}

void Resolve(MotionEvaluatedNode& node, const MotionHomography& parent) {
    const MotionHomography transform = parent.After(MotionNodeMapping(node));
    if (!node.filters.empty()) {
        const MotionBox bounds = MotionSubtreeBounds(node, parent);
        const double reach =
            MotionFilterReach(node.filters) *
            transform.LocalScale(node.box.x + node.box.width / 2.0,
                                 node.box.y + node.box.height / 2.0);
        node.has_filter_domain = true;
        node.filter_domain = {bounds.x - reach, bounds.y - reach,
                              bounds.width + 2.0 * reach,
                              bounds.height + 2.0 * reach};
    }
    if (node.has_3d) {
        const double xs[] = {node.box.x, node.box.x + node.box.width,
                             node.box.x + node.box.width, node.box.x};
        const double ys[] = {node.box.y, node.box.y,
                             node.box.y + node.box.height,
                             node.box.y + node.box.height};
        node.has_quad = true;
        for (int corner = 0; corner < 4; ++corner) {
            if (!transform.Apply(xs[corner], ys[corner],
                                 node.quad[corner].first,
                                 node.quad[corner].second)) {
                node.has_quad = false;
            }
        }
    }
    for (MotionEvaluatedNode& child : node.children) {
        Resolve(child, transform);
    }
    if (!node.instance_root.empty()) {
        Resolve(node.instance_root.front(),
                transform.After(MotionHomography::FromAffine(
                    MotionInstanceTransform(node))));
    }
}

// Row-major 4x4 product: (left * right).
void Multiply4(const double left[16], const double right[16], double out[16]) {
    double result[16];
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            double sum = 0.0;
            for (int k = 0; k < 4; ++k) {
                sum += left[row * 4 + k] * right[k * 4 + column];
            }
            result[row * 4 + column] = sum;
        }
    }
    std::copy(result, result + 16, out);
}

void Identity4(double out[16]) {
    std::fill(out, out + 16, 0.0);
    out[0] = out[5] = out[10] = out[15] = 1.0;
}

void Translate4(double x, double y, double out[16]) {
    Identity4(out);
    out[3] = x;
    out[7] = y;
}

}  // namespace

MotionAffine MotionAffine::After(const MotionAffine& inner) const {
    // (this ∘ inner)(p) = this(inner(p)).
    MotionAffine out;
    out.a = a * inner.a + c * inner.b;
    out.b = b * inner.a + d * inner.b;
    out.c = a * inner.c + c * inner.d;
    out.d = b * inner.c + d * inner.d;
    out.tx = a * inner.tx + c * inner.ty + tx;
    out.ty = b * inner.tx + d * inner.ty + ty;
    return out;
}

MotionAffine MotionAffine::Inverse() const {
    const double determinant = a * d - b * c;
    MotionAffine out;
    if (determinant == 0.0) {
        return out;
    }
    out.a = d / determinant;
    out.b = -b / determinant;
    out.c = -c / determinant;
    out.d = a / determinant;
    out.tx = -(out.a * tx + out.c * ty);
    out.ty = -(out.b * tx + out.d * ty);
    return out;
}

void MotionAffine::Apply(double x, double y, double& outX, double& outY) const {
    outX = a * x + c * y + tx;
    outY = b * x + d * y + ty;
}

double MotionAffine::LengthScale() const {
    return std::sqrt(std::abs(a * d - b * c));
}

MotionHomography MotionHomography::FromAffine(const MotionAffine& affine) {
    MotionHomography out;
    out.m[0] = affine.a;
    out.m[1] = affine.c;
    out.m[2] = affine.tx;
    out.m[3] = affine.b;
    out.m[4] = affine.d;
    out.m[5] = affine.ty;
    return out;
}

MotionHomography MotionHomography::After(const MotionHomography& inner) const {
    MotionHomography out;
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            double sum = 0.0;
            for (int k = 0; k < 3; ++k) {
                sum += m[row * 3 + k] * inner.m[k * 3 + column];
            }
            out.m[row * 3 + column] = sum;
        }
    }
    return out;
}

MotionHomography MotionHomography::Inverse() const {
    const double* a = m;
    const double c00 = a[4] * a[8] - a[5] * a[7];
    const double c01 = a[5] * a[6] - a[3] * a[8];
    const double c02 = a[3] * a[7] - a[4] * a[6];
    const double determinant = a[0] * c00 + a[1] * c01 + a[2] * c02;
    MotionHomography out;
    if (determinant == 0.0) {
        return out;
    }
    out.m[0] = c00 / determinant;
    out.m[1] = (a[2] * a[7] - a[1] * a[8]) / determinant;
    out.m[2] = (a[1] * a[5] - a[2] * a[4]) / determinant;
    out.m[3] = c01 / determinant;
    out.m[4] = (a[0] * a[8] - a[2] * a[6]) / determinant;
    out.m[5] = (a[2] * a[3] - a[0] * a[5]) / determinant;
    out.m[6] = c02 / determinant;
    out.m[7] = (a[1] * a[6] - a[0] * a[7]) / determinant;
    out.m[8] = (a[0] * a[4] - a[1] * a[3]) / determinant;
    return out;
}

bool MotionHomography::Apply(double x, double y, double& outX,
                             double& outY) const {
    const double w = m[6] * x + m[7] * y + m[8];
    if (w <= 1e-12) {
        return false;
    }
    outX = (m[0] * x + m[1] * y + m[2]) / w;
    outY = (m[3] * x + m[4] * y + m[5]) / w;
    return true;
}

double MotionHomography::LocalScale(double x, double y) const {
    const double w = m[6] * x + m[7] * y + m[8];
    if (w <= 1e-12) {
        return 1.0;
    }
    const double u = m[0] * x + m[1] * y + m[2];
    const double v = m[3] * x + m[4] * y + m[5];
    // Jacobian of (u / w, v / w).
    const double dudx = (m[0] * w - u * m[6]) / (w * w);
    const double dudy = (m[1] * w - u * m[7]) / (w * w);
    const double dvdx = (m[3] * w - v * m[6]) / (w * w);
    const double dvdy = (m[4] * w - v * m[7]) / (w * w);
    return std::sqrt(std::abs(dudx * dvdy - dudy * dvdx));
}

void MotionNodeMatrix3D(const MotionEvaluatedNode& node, double perspective,
                        double originX, double originY, double matrix[16]) {
    const double centerX = node.box.x + node.box.width / 2.0;
    const double centerY = node.box.y + node.box.height / 2.0;
    const double ax = node.rotate_x * kPi / 180.0;
    const double ay = node.rotate_y * kPi / 180.0;
    const double az = node.rotate * kPi / 180.0;
    double result[16];
    Translate4(centerX + node.translate_x, centerY + node.translate_y, result);
    // rotateX, rotateY, rotate (about z), scale, in CSS's function order.
    const double rotateX[16] = {1,
                                0,
                                0,
                                0,
                                0,
                                std::cos(ax),
                                -std::sin(ax),
                                0,
                                0,
                                std::sin(ax),
                                std::cos(ax),
                                0,
                                0,
                                0,
                                0,
                                1};
    const double rotateY[16] = {std::cos(ay),  0, std::sin(ay), 0, 0, 1, 0, 0,
                                -std::sin(ay), 0, std::cos(ay), 0, 0, 0, 0, 1};
    const double rotateZ[16] = {std::cos(az),
                                -std::sin(az),
                                0,
                                0,
                                std::sin(az),
                                std::cos(az),
                                0,
                                0,
                                0,
                                0,
                                1,
                                0,
                                0,
                                0,
                                0,
                                1};
    const double scale[16] = {
        node.scale_x, 0, 0, 0, 0, node.scale_y, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    double back[16];
    Translate4(-centerX, -centerY, back);
    Multiply4(result, rotateX, result);
    Multiply4(result, rotateY, result);
    Multiply4(result, rotateZ, result);
    Multiply4(result, scale, result);
    Multiply4(result, back, result);
    if (perspective > 0.0) {
        // CSS perspective(d) about the parent's centre: w = 1 - z / d.
        double to[16];
        double depth[16];
        double from[16];
        Translate4(originX, originY, to);
        Identity4(depth);
        depth[14] = -1.0 / perspective;
        Translate4(-originX, -originY, from);
        Multiply4(depth, from, depth);
        Multiply4(to, depth, depth);
        Multiply4(depth, result, result);
    }
    std::copy(result, result + 16, matrix);
}

MotionHomography MotionPlaneHomography(const double matrix[16]) {
    // Columns x, y and w of the rows x, y and w: the plane z = 0.
    MotionHomography out;
    const int rows[] = {0, 1, 3};
    const int columns[] = {0, 1, 3};
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            out.m[row * 3 + column] = matrix[rows[row] * 4 + columns[column]];
        }
    }
    return out;
}

MotionHomography MotionNodeMapping(const MotionEvaluatedNode& node) {
    if (node.has_3d) {
        return MotionPlaneHomography(node.matrix3d);
    }
    return MotionHomography::FromAffine(MotionNodeTransform(node));
}

MotionAffine MotionNodeTransform(const MotionEvaluatedNode& node) {
    // Translate, then rotate, then scale, around the centre of the box: the
    // painter's Transform (MotionRender.mm), term for term.
    const double centerX = node.box.x + node.box.width / 2.0;
    const double centerY = node.box.y + node.box.height / 2.0;
    const double radians = node.rotate * kPi / 180.0;
    MotionAffine rotateScale;
    rotateScale.a = std::cos(radians) * node.scale_x;
    rotateScale.b = std::sin(radians) * node.scale_x;
    rotateScale.c = -std::sin(radians) * node.scale_y;
    rotateScale.d = std::cos(radians) * node.scale_y;
    return Translation(centerX + node.translate_x, centerY + node.translate_y)
        .After(rotateScale)
        .After(Translation(-centerX, -centerY));
}

MotionAffine MotionInstanceTransform(const MotionEvaluatedNode& node) {
    MotionAffine affine;
    affine.a = node.instance.scale_x;
    affine.d = node.instance.scale_y;
    affine.tx = node.instance.origin_x;
    affine.ty = node.instance.origin_y;
    return affine;
}

MotionBox MotionSubtreeBounds(const MotionEvaluatedNode& node,
                              const MotionAffine& parent) {
    return MotionSubtreeBounds(node, MotionHomography::FromAffine(parent));
}

MotionBox MotionSubtreeBounds(const MotionEvaluatedNode& node,
                              const MotionHomography& parent) {
    Extent extent;
    Collect(node, parent, extent);
    if (extent.Empty()) {
        return {};
    }
    return {extent.min_x, extent.min_y, extent.max_x - extent.min_x,
            extent.max_y - extent.min_y};
}

double MotionFilterReach(const std::vector<MotionFilter>& filters) {
    double reach = 0.0;
    for (const MotionFilter& filter : filters) {
        switch (filter.kind) {
            case MotionFilterKind::Blur:
            case MotionFilterKind::Glow:
                reach += 3.0 * filter.sigma;
                break;
            case MotionFilterKind::DropShadow:
                reach += std::max(std::abs(filter.offset_x),
                                  std::abs(filter.offset_y)) +
                         3.0 * filter.sigma;
                break;
            case MotionFilterKind::Displace:
            case MotionFilterKind::Slice:
                reach += filter.amplitude;
                break;
            case MotionFilterKind::Grain:
                break;
            case MotionFilterKind::Goo:
                reach += 3.0 * filter.sigma;
                break;
            case MotionFilterKind::Mesh: {
                // Each cell stays within the hull of its vertices and of
                // handles that travel with them: the farthest a vertex
                // moved from its rest bounds how far the picture goes.
                double farthest = 0.0;
                const int32_t columns = filter.mesh_columns + 1;
                for (size_t vertex = 0; vertex < filter.mesh_resolved.size();
                     ++vertex) {
                    double restX = 0.0;
                    double restY = 0.0;
                    MotionMeshRest(
                        filter, static_cast<int32_t>(vertex) / columns,
                        static_cast<int32_t>(vertex) % columns, restX, restY);
                    farthest = std::max(
                        {farthest,
                         std::abs(filter.mesh_resolved[vertex].first - restX),
                         std::abs(filter.mesh_resolved[vertex].second -
                                  restY)});
                }
                reach += farthest;
                break;
            }
        }
    }
    return reach;
}

void ResolveMotionEffectDomains(MotionFrame& frame) {
    Resolve(frame.root, MotionHomography{});
}
