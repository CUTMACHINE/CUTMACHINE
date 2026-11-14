#pragma once

// MOTION-2026-09 M1b -- ROADMAP.md, docs/MOTION_FORMAT.md. Vector shapes for
// motion compositions: SVG path data or a list of points, reduced to moves,
// lines and cubic curves; length, trimming and bounds computed here so the
// geometry a caller reads (--describe-motion-frame) is the geometry Core
// Graphics draws. Portable and pixel-agnostic: coordinates are doubles in the
// node's own space, and nothing here touches a framework.

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

enum class MotionPathVerb { Move, Line, Cubic, Close };

// A Cubic uses both control points; Move and Line only the end point; Close
// returns to the start of the current subpath.
struct MotionPathSegment {
    MotionPathVerb verb = MotionPathVerb::Move;
    double x1 = 0.0;
    double y1 = 0.0;
    double x2 = 0.0;
    double y2 = 0.0;
    double x = 0.0;
    double y = 0.0;
};

using MotionPath = std::vector<MotionPathSegment>;

struct MotionPathBounds {
    double min_x = 0.0;
    double min_y = 0.0;
    double max_x = 0.0;
    double max_y = 0.0;
};

enum class MotionPathCurve { Linear, CatmullRom };

// SVG path data (the `d` attribute): M L H V C S Q T A Z in both absolute
// and relative forms, with SVG's number grammar. Quadratic curves and arcs
// become cubic curves, so the rest of the pipeline knows three verbs. An
// error names the offending position; nothing is guessed.
bool ParseMotionSvgPath(const std::string& data, MotionPath& path,
                        std::string& error);

// A polyline or a uniform Catmull-Rom spline through `points`, closed on
// request. Catmull-Rom passes through every point, which is what a list of
// points drawn by hand means.
MotionPath MotionPathFromPoints(
    const std::vector<std::pair<double, double>>& points, MotionPathCurve curve,
    bool closed);

// Length of every drawn segment, closing segments included, over all
// subpaths in order. Curves are measured on a fixed subdivision so the same
// path always gives the same length.
double MotionPathLength(const MotionPath& path);

// The part of the path between the fractions `start` and `end` of its total
// length (swapped when start > end, as a trim in a motion tool does). A
// closed subpath kept whole stays closed; a cut one becomes open.
MotionPath TrimMotionPath(const MotionPath& path, double start, double end);

// Exact bounds of the drawn geometry (curve extrema included, stroke width
// excluded, as SVG getBBox). False for a path that draws nothing.
bool MotionPathBoundsOf(const MotionPath& path, MotionPathBounds& bounds);

// MOTION-DESIGN-2026-09 -- path morphing. Two paths morph when they have
// the same structure: the same moves and closes at the same places, and as
// many drawn segments in between -- a line counting as a cubic whose handles
// lie on it, so a straight edge can bend into a curve. Mixing is point by
// point; t outside [0, 1] extrapolates, which an overshooting curve asks.
bool MotionPathsMorphable(const MotionPath& a, const MotionPath& b);
MotionPath MixMotionPaths(const MotionPath& a, const MotionPath& b, double t);

// Scales then translates every coordinate: the viewBox mapping and the move
// into composition pixels.
void TransformMotionPath(MotionPath& path, double scale, double dx, double dy);
