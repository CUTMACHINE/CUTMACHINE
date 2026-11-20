#include "MotionPath.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <locale>
#include <sstream>

namespace {

constexpr double kPi = 3.14159265358979323846;

// Curves are measured on this many chords. Fixed, so a length or a trim
// point never depends on the machine or on the scale of the drawing.
constexpr int kCurveSteps = 64;

struct Point {
    double x = 0.0;
    double y = 0.0;
};

Point Lerp(Point a, Point b, double t) {
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
}

double Distance(Point a, Point b) { return std::hypot(b.x - a.x, b.y - a.y); }

Point CubicPoint(Point p0, Point p1, Point p2, Point p3, double t) {
    const double u = 1.0 - t;
    const double a = u * u * u;
    const double b = 3.0 * u * u * t;
    const double c = 3.0 * u * t * t;
    const double d = t * t * t;
    return {a * p0.x + b * p1.x + c * p2.x + d * p3.x,
            a * p0.y + b * p1.y + c * p2.y + d * p3.y};
}

class Scanner {
public:
    explicit Scanner(const std::string& text) : text_(text) {}

    bool AtEnd() {
        Skip();
        return position_ >= text_.size();
    }

    size_t position() const { return position_; }

    char Peek() {
        Skip();
        return position_ < text_.size() ? text_[position_] : '\0';
    }

    void Advance() { ++position_; }

    bool AtNumber() {
        const char c = Peek();
        return (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.';
    }

    // SVG number grammar: sign, digits with an optional fraction or a bare
    // fraction, optional exponent. "1.5.5" is 1.5 then .5; "1-2" is 1 then
    // -2, as every SVG reader does.
    bool Number(double& value) {
        Skip();
        const size_t start = position_;
        if (position_ < text_.size() &&
            (text_[position_] == '+' || text_[position_] == '-')) {
            ++position_;
        }
        size_t digits = Digits();
        if (position_ < text_.size() && text_[position_] == '.') {
            ++position_;
            digits += Digits();
        }
        if (digits == 0) {
            position_ = start;
            return false;
        }
        if (position_ < text_.size() &&
            (text_[position_] == 'e' || text_[position_] == 'E')) {
            const size_t mark = position_;
            ++position_;
            if (position_ < text_.size() &&
                (text_[position_] == '+' || text_[position_] == '-')) {
                ++position_;
            }
            if (Digits() == 0) {
                position_ = mark;
            }
        }
        std::istringstream stream(text_.substr(start, position_ - start));
        stream.imbue(std::locale::classic());
        stream >> value;
        return !stream.fail() && std::isfinite(value);
    }

    // Arc flags are a single digit and may touch the next number ("a1 1 0
    // 00 5 5").
    bool Flag(bool& value) {
        Skip();
        if (position_ < text_.size() &&
            (text_[position_] == '0' || text_[position_] == '1')) {
            value = text_[position_] == '1';
            ++position_;
            return true;
        }
        return false;
    }

private:
    void Skip() {
        while (
            position_ < text_.size() &&
            (std::isspace(static_cast<unsigned char>(text_[position_])) != 0 ||
             text_[position_] == ',')) {
            ++position_;
        }
    }

    size_t Digits() {
        size_t count = 0;
        while (position_ < text_.size() && text_[position_] >= '0' &&
               text_[position_] <= '9') {
            ++position_;
            ++count;
        }
        return count;
    }

    const std::string& text_;
    size_t position_ = 0;
};

MotionPathSegment MoveTo(Point p) {
    MotionPathSegment segment;
    segment.verb = MotionPathVerb::Move;
    segment.x = p.x;
    segment.y = p.y;
    return segment;
}

MotionPathSegment LineTo(Point p) {
    MotionPathSegment segment;
    segment.verb = MotionPathVerb::Line;
    segment.x = p.x;
    segment.y = p.y;
    return segment;
}

MotionPathSegment CubicTo(Point c1, Point c2, Point p) {
    MotionPathSegment segment;
    segment.verb = MotionPathVerb::Cubic;
    segment.x1 = c1.x;
    segment.y1 = c1.y;
    segment.x2 = c2.x;
    segment.y2 = c2.y;
    segment.x = p.x;
    segment.y = p.y;
    return segment;
}

MotionPathSegment ClosePath() {
    MotionPathSegment segment;
    segment.verb = MotionPathVerb::Close;
    return segment;
}

// SVG implementation notes, "elliptical arc implementation": endpoint to
// centre parameterisation, then one cubic per quarter turn at most.
void AppendArc(MotionPath& path, Point from, double rx, double ry,
               double rotation, bool large, bool sweep, Point to) {
    if (from.x == to.x && from.y == to.y) {
        return;
    }
    rx = std::fabs(rx);
    ry = std::fabs(ry);
    if (rx == 0.0 || ry == 0.0) {
        path.push_back(LineTo(to));
        return;
    }
    const double phi = rotation * kPi / 180.0;
    const double cosPhi = std::cos(phi);
    const double sinPhi = std::sin(phi);
    const double dx = (from.x - to.x) / 2.0;
    const double dy = (from.y - to.y) / 2.0;
    const double x1 = cosPhi * dx + sinPhi * dy;
    const double y1 = -sinPhi * dx + cosPhi * dy;
    const double lambda = (x1 * x1) / (rx * rx) + (y1 * y1) / (ry * ry);
    if (lambda > 1.0) {
        rx *= std::sqrt(lambda);
        ry *= std::sqrt(lambda);
    }
    const double numerator =
        rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1;
    const double denominator = rx * rx * y1 * y1 + ry * ry * x1 * x1;
    double coefficient = denominator > 0.0
                             ? std::sqrt(std::max(0.0, numerator / denominator))
                             : 0.0;
    if (large == sweep) {
        coefficient = -coefficient;
    }
    const double cxPrime = coefficient * rx * y1 / ry;
    const double cyPrime = -coefficient * ry * x1 / rx;
    const double cx = cosPhi * cxPrime - sinPhi * cyPrime + (from.x + to.x) / 2;
    const double cy = sinPhi * cxPrime + cosPhi * cyPrime + (from.y + to.y) / 2;
    const auto angle = [](double ux, double uy, double vx, double vy) {
        return std::atan2(ux * vy - uy * vx, ux * vx + uy * vy);
    };
    const double theta =
        angle(1.0, 0.0, (x1 - cxPrime) / rx, (y1 - cyPrime) / ry);
    double delta = angle((x1 - cxPrime) / rx, (y1 - cyPrime) / ry,
                         (-x1 - cxPrime) / rx, (-y1 - cyPrime) / ry);
    if (!sweep && delta > 0.0) {
        delta -= 2.0 * kPi;
    } else if (sweep && delta < 0.0) {
        delta += 2.0 * kPi;
    }
    const int count = std::max(
        1, static_cast<int>(std::ceil(std::fabs(delta) / (kPi / 2.0) - 1e-9)));
    const double step = delta / count;
    const double k = 4.0 / 3.0 * std::tan(step / 4.0);
    const auto at = [&](double t) {
        return Point{
            cx + rx * std::cos(t) * cosPhi - ry * std::sin(t) * sinPhi,
            cy + rx * std::cos(t) * sinPhi + ry * std::sin(t) * cosPhi};
    };
    const auto slope = [&](double t) {
        return Point{-rx * std::sin(t) * cosPhi - ry * std::cos(t) * sinPhi,
                     -rx * std::sin(t) * sinPhi + ry * std::cos(t) * cosPhi};
    };
    Point start = from;
    for (int index = 0; index < count; ++index) {
        const double t1 = theta + step * index;
        const double t2 = t1 + step;
        const Point end = index + 1 == count ? to : at(t2);
        const Point d1 = slope(t1);
        const Point d2 = slope(t2);
        path.push_back(CubicTo({start.x + k * d1.x, start.y + k * d1.y},
                               {end.x - k * d2.x, end.y - k * d2.y}, end));
        start = end;
    }
}

// One drawn piece of a path, with its arc-length table for curves.
struct Piece {
    bool cubic = false;
    Point p0;
    Point p1;
    Point p2;
    Point p3;
    size_t subpath = 0;
    double start = 0.0;
    double length = 0.0;
    std::vector<double> table;  // arc length at t = i / kCurveSteps

    Point At(double t) const {
        return cubic ? CubicPoint(p0, p1, p2, p3, t) : Lerp(p0, p3, t);
    }

    double ParameterAt(double distance) const {
        if (length <= 0.0) {
            return 0.0;
        }
        if (!cubic) {
            return std::clamp(distance / length, 0.0, 1.0);
        }
        const auto upper =
            std::lower_bound(table.begin(), table.end(), distance);
        if (upper == table.begin()) {
            return 0.0;
        }
        if (upper == table.end()) {
            return 1.0;
        }
        const size_t index = static_cast<size_t>(upper - table.begin());
        const double before = table[index - 1];
        const double span = *upper - before;
        const double fraction = span > 0.0 ? (distance - before) / span : 0.0;
        return (static_cast<double>(index - 1) + fraction) / kCurveSteps;
    }
};

struct Subpath {
    size_t first_segment = 0;
    size_t end_segment = 0;  // one past the last segment, Close included
    bool closed = false;
    double start = 0.0;
    double end = 0.0;
};

void Decompose(const MotionPath& path, std::vector<Piece>& pieces,
               std::vector<Subpath>& subpaths) {
    Point current;
    Point origin;
    double total = 0.0;
    bool open = false;
    const auto add = [&](Piece piece) {
        piece.subpath = subpaths.size() - 1;
        piece.start = total;
        if (piece.cubic) {
            piece.table.push_back(0.0);
            Point previous = piece.p0;
            double length = 0.0;
            for (int step = 1; step <= kCurveSteps; ++step) {
                const Point next =
                    CubicPoint(piece.p0, piece.p1, piece.p2, piece.p3,
                               static_cast<double>(step) / kCurveSteps);
                length += Distance(previous, next);
                piece.table.push_back(length);
                previous = next;
            }
            piece.length = length;
        } else {
            piece.length = Distance(piece.p0, piece.p3);
        }
        total += piece.length;
        pieces.push_back(std::move(piece));
    };
    for (size_t index = 0; index < path.size(); ++index) {
        const MotionPathSegment& segment = path[index];
        if (segment.verb == MotionPathVerb::Move || !open) {
            if (open) {
                subpaths.back().end_segment = index;
                subpaths.back().end = total;
            }
            Subpath subpath;
            subpath.first_segment = index;
            subpath.start = total;
            subpaths.push_back(subpath);
            open = true;
            if (segment.verb == MotionPathVerb::Move) {
                current = origin = {segment.x, segment.y};
                continue;
            }
            origin = current;
        }
        Piece piece;
        piece.p0 = current;
        if (segment.verb == MotionPathVerb::Close) {
            piece.p3 = origin;
            add(piece);
            current = origin;
            subpaths.back().closed = true;
            subpaths.back().end_segment = index + 1;
            subpaths.back().end = total;
            open = false;
            continue;
        }
        piece.p3 = {segment.x, segment.y};
        if (segment.verb == MotionPathVerb::Cubic) {
            piece.cubic = true;
            piece.p1 = {segment.x1, segment.y1};
            piece.p2 = {segment.x2, segment.y2};
        }
        add(piece);
        current = piece.p3;
    }
    if (open) {
        subpaths.back().end_segment = path.size();
        subpaths.back().end = total;
    }
}

// The part of a cubic between two parameters, by de Casteljau.
void SplitCubic(const Piece& piece, double t0, double t1, Point& c1, Point& c2,
                Point& end) {
    // Keep [0, t1] first.
    const Point a = Lerp(piece.p0, piece.p1, t1);
    const Point b = Lerp(piece.p1, piece.p2, t1);
    const Point c = Lerp(piece.p2, piece.p3, t1);
    const Point ab = Lerp(a, b, t1);
    const Point bc = Lerp(b, c, t1);
    const Point head = Lerp(ab, bc, t1);
    // Then [t0 / t1, 1] of that head curve.
    const double s = t1 > 0.0 ? t0 / t1 : 0.0;
    const Point q1 = a;
    const Point q2 = ab;
    const Point q3 = head;
    const Point e = Lerp(q1, q2, s);
    const Point f = Lerp(q2, q3, s);
    const Point ef = Lerp(e, f, s);
    c1 = ef;
    c2 = f;
    end = q3;
}

void Include(MotionPathBounds& bounds, bool& any, Point p) {
    if (!any) {
        bounds = {p.x, p.y, p.x, p.y};
        any = true;
        return;
    }
    bounds.min_x = std::min(bounds.min_x, p.x);
    bounds.min_y = std::min(bounds.min_y, p.y);
    bounds.max_x = std::max(bounds.max_x, p.x);
    bounds.max_y = std::max(bounds.max_y, p.y);
}

// Parameters in (0, 1) where one coordinate of a cubic has a zero slope.
std::vector<double> Extrema(double p0, double p1, double p2, double p3) {
    const double a = p1 - p0;
    const double b = p2 - p1;
    const double c = p3 - p2;
    const double qa = a - 2.0 * b + c;
    const double qb = 2.0 * (b - a);
    const double qc = a;
    std::vector<double> roots;
    if (std::fabs(qa) < 1e-12) {
        if (std::fabs(qb) > 1e-12) {
            roots.push_back(-qc / qb);
        }
    } else {
        const double discriminant = qb * qb - 4.0 * qa * qc;
        if (discriminant >= 0.0) {
            const double root = std::sqrt(discriminant);
            roots.push_back((-qb + root) / (2.0 * qa));
            roots.push_back((-qb - root) / (2.0 * qa));
        }
    }
    std::vector<double> inside;
    for (double root : roots) {
        if (root > 0.0 && root < 1.0) {
            inside.push_back(root);
        }
    }
    return inside;
}

}  // namespace

bool ParseMotionSvgPath(const std::string& data, MotionPath& path,
                        std::string& error) {
    Scanner scanner(data);
    MotionPath parsed;
    Point current;
    Point origin;
    Point lastCubic;
    Point lastQuad;
    char previous = '\0';
    char command = '\0';
    bool closedSinceMove = false;
    const auto fail = [&](const std::string& message) {
        error =
            message + " at character " + std::to_string(scanner.position() + 1);
        return false;
    };
    while (!scanner.AtEnd()) {
        const char next = scanner.Peek();
        if (std::isalpha(static_cast<unsigned char>(next)) != 0) {
            command = next;
            scanner.Advance();
        } else if (command == '\0') {
            return fail("path data must start with a moveto (M or m)");
        } else if (command == 'Z' || command == 'z') {
            return fail("a closepath takes no numbers");
        }
        // Implicit repetition: numbers after a moveto are linetos.
        const char upper = static_cast<char>(
            std::toupper(static_cast<unsigned char>(command)));
        const bool relative = command != upper;
        if (parsed.empty() && upper != 'M') {
            return fail("path data must start with a moveto (M or m)");
        }
        if (upper != 'M' && upper != 'Z' && closedSinceMove) {
            // After a closepath, the next subpath starts where the closed
            // one did.
            parsed.push_back(MoveTo(origin));
            closedSinceMove = false;
        }
        const auto point = [&](Point& p) {
            if (!scanner.Number(p.x) || !scanner.Number(p.y)) {
                return false;
            }
            if (relative) {
                p.x += current.x;
                p.y += current.y;
            }
            return true;
        };
        switch (upper) {
            case 'M': {
                Point p;
                if (!point(p)) {
                    return fail("moveto needs x y");
                }
                parsed.push_back(MoveTo(p));
                current = origin = p;
                closedSinceMove = false;
                command = relative ? 'l' : 'L';
                previous = 'M';
                continue;
            }
            case 'Z':
                parsed.push_back(ClosePath());
                current = origin;
                closedSinceMove = true;
                previous = 'Z';
                // A command letter must follow; numbers are refused at the
                // top of the loop.
                continue;
            case 'L': {
                Point p;
                if (!point(p)) {
                    return fail("lineto needs x y");
                }
                parsed.push_back(LineTo(p));
                current = p;
                break;
            }
            case 'H':
            case 'V': {
                double value = 0.0;
                if (!scanner.Number(value)) {
                    return fail(std::string(1, upper) + " needs a coordinate");
                }
                Point p = current;
                if (upper == 'H') {
                    p.x = relative ? current.x + value : value;
                } else {
                    p.y = relative ? current.y + value : value;
                }
                parsed.push_back(LineTo(p));
                current = p;
                break;
            }
            case 'C':
            case 'S': {
                Point c1;
                Point c2;
                Point p;
                if (upper == 'C') {
                    if (!point(c1)) {
                        return fail("curveto needs x1 y1 x2 y2 x y");
                    }
                } else if (previous == 'C' || previous == 'S') {
                    c1 = {2.0 * current.x - lastCubic.x,
                          2.0 * current.y - lastCubic.y};
                } else {
                    c1 = current;
                }
                if (!point(c2) || !point(p)) {
                    return fail(upper == 'C'
                                    ? "curveto needs x1 y1 x2 y2 x y"
                                    : "smooth curveto needs x2 y2 x y");
                }
                parsed.push_back(CubicTo(c1, c2, p));
                lastCubic = c2;
                current = p;
                break;
            }
            case 'Q':
            case 'T': {
                Point q;
                Point p;
                if (upper == 'Q') {
                    if (!point(q)) {
                        return fail("quadratic curveto needs x1 y1 x y");
                    }
                } else if (previous == 'Q' || previous == 'T') {
                    q = {2.0 * current.x - lastQuad.x,
                         2.0 * current.y - lastQuad.y};
                } else {
                    q = current;
                }
                if (!point(p)) {
                    return fail(upper == 'Q'
                                    ? "quadratic curveto needs x1 y1 x y"
                                    : "smooth quadratic curveto needs x y");
                }
                // A quadratic is exactly a cubic with these controls.
                parsed.push_back(
                    CubicTo({current.x + 2.0 / 3.0 * (q.x - current.x),
                             current.y + 2.0 / 3.0 * (q.y - current.y)},
                            {p.x + 2.0 / 3.0 * (q.x - p.x),
                             p.y + 2.0 / 3.0 * (q.y - p.y)},
                            p));
                lastQuad = q;
                current = p;
                break;
            }
            case 'A': {
                double rx = 0.0;
                double ry = 0.0;
                double rotation = 0.0;
                bool large = false;
                bool sweep = false;
                Point p;
                if (!scanner.Number(rx) || !scanner.Number(ry) ||
                    !scanner.Number(rotation) || !scanner.Flag(large) ||
                    !scanner.Flag(sweep) || !point(p)) {
                    return fail(
                        "arc needs rx ry rotation large-arc-flag sweep-flag "
                        "x y");
                }
                AppendArc(parsed, current, rx, ry, rotation, large, sweep, p);
                current = p;
                break;
            }
            default:
                return fail(std::string("unknown path command '") + command +
                            "'");
        }
        previous = upper;
    }
    path = std::move(parsed);
    return true;
}

MotionPath MotionPathFromPoints(
    const std::vector<std::pair<double, double>>& points, MotionPathCurve curve,
    bool closed) {
    MotionPath path;
    if (points.empty()) {
        return path;
    }
    const auto at = [&](long index) {
        const long count = static_cast<long>(points.size());
        if (closed) {
            index = ((index % count) + count) % count;
        } else {
            index = std::clamp(index, 0L, count - 1);
        }
        return Point{points[static_cast<size_t>(index)].first,
                     points[static_cast<size_t>(index)].second};
    };
    path.push_back(MoveTo(at(0)));
    const long count = static_cast<long>(points.size());
    const long segments = closed ? count : count - 1;
    for (long index = 0; index < segments; ++index) {
        const Point p1 = at(index);
        const Point p2 = at(index + 1);
        if (curve == MotionPathCurve::Linear) {
            path.push_back(LineTo(p2));
            continue;
        }
        const Point p0 = at(index - 1);
        const Point p3 = at(index + 2);
        path.push_back(CubicTo(
            {p1.x + (p2.x - p0.x) / 6.0, p1.y + (p2.y - p0.y) / 6.0},
            {p2.x - (p3.x - p1.x) / 6.0, p2.y - (p3.y - p1.y) / 6.0}, p2));
    }
    if (closed) {
        path.push_back(ClosePath());
    }
    return path;
}

namespace {

// Every drawn segment as a cubic: a line's handles at its thirds.
MotionPath AsCubics(const MotionPath& path) {
    MotionPath out;
    out.reserve(path.size());
    double x = 0.0;
    double y = 0.0;
    double startX = 0.0;
    double startY = 0.0;
    for (const MotionPathSegment& segment : path) {
        MotionPathSegment cubic = segment;
        switch (segment.verb) {
            case MotionPathVerb::Move:
                startX = segment.x;
                startY = segment.y;
                break;
            case MotionPathVerb::Line:
                cubic.verb = MotionPathVerb::Cubic;
                cubic.x1 = x + (segment.x - x) / 3.0;
                cubic.y1 = y + (segment.y - y) / 3.0;
                cubic.x2 = x + 2.0 * (segment.x - x) / 3.0;
                cubic.y2 = y + 2.0 * (segment.y - y) / 3.0;
                break;
            case MotionPathVerb::Cubic:
                break;
            case MotionPathVerb::Close:
                cubic.x = startX;
                cubic.y = startY;
                break;
        }
        out.push_back(cubic);
        if (segment.verb == MotionPathVerb::Close) {
            x = startX;
            y = startY;
        } else {
            x = segment.x;
            y = segment.y;
        }
    }
    return out;
}

bool Drawn(MotionPathVerb verb) {
    return verb == MotionPathVerb::Line || verb == MotionPathVerb::Cubic;
}

}  // namespace

bool MotionPathsMorphable(const MotionPath& a, const MotionPath& b) {
    if (a.size() != b.size() || a.empty()) {
        return false;
    }
    for (size_t index = 0; index < a.size(); ++index) {
        const MotionPathVerb left = a[index].verb;
        const MotionPathVerb right = b[index].verb;
        if (left != right && !(Drawn(left) && Drawn(right))) {
            return false;
        }
    }
    return true;
}

MotionPath MixMotionPaths(const MotionPath& a, const MotionPath& b, double t) {
    if (!MotionPathsMorphable(a, b)) {
        return a;
    }
    const MotionPath from = AsCubics(a);
    const MotionPath to = AsCubics(b);
    MotionPath out = from;
    const auto mix = [t](double p, double q) { return p + (q - p) * t; };
    for (size_t index = 0; index < out.size(); ++index) {
        MotionPathSegment& segment = out[index];
        const MotionPathSegment& other = to[index];
        segment.x1 = mix(segment.x1, other.x1);
        segment.y1 = mix(segment.y1, other.y1);
        segment.x2 = mix(segment.x2, other.x2);
        segment.y2 = mix(segment.y2, other.y2);
        segment.x = mix(segment.x, other.x);
        segment.y = mix(segment.y, other.y);
    }
    return out;
}

double MotionPathLength(const MotionPath& path) {
    std::vector<Piece> pieces;
    std::vector<Subpath> subpaths;
    Decompose(path, pieces, subpaths);
    double length = 0.0;
    for (const Piece& piece : pieces) {
        length += piece.length;
    }
    return length;
}

MotionPath TrimMotionPath(const MotionPath& path, double start, double end) {
    start = std::clamp(start, 0.0, 1.0);
    end = std::clamp(end, 0.0, 1.0);
    if (start > end) {
        std::swap(start, end);
    }
    if (start == 0.0 && end == 1.0) {
        return path;
    }
    std::vector<Piece> pieces;
    std::vector<Subpath> subpaths;
    Decompose(path, pieces, subpaths);
    double total = 0.0;
    for (const Piece& piece : pieces) {
        total += piece.length;
    }
    MotionPath trimmed;
    if (total <= 0.0 || start == end) {
        return trimmed;
    }
    const double from = start * total;
    const double to = end * total;
    size_t pieceIndex = 0;
    for (size_t subpathIndex = 0; subpathIndex < subpaths.size();
         ++subpathIndex) {
        const Subpath& subpath = subpaths[subpathIndex];
        const size_t first = pieceIndex;
        while (pieceIndex < pieces.size() &&
               pieces[pieceIndex].subpath == subpathIndex) {
            ++pieceIndex;
        }
        if (subpath.end <= from || subpath.start >= to) {
            continue;
        }
        if (subpath.start >= from && subpath.end <= to) {
            // Whole: keep it verbatim, closepath and joins included.
            trimmed.insert(
                trimmed.end(),
                path.begin() + static_cast<long>(subpath.first_segment),
                path.begin() + static_cast<long>(subpath.end_segment));
            continue;
        }
        bool drawing = false;
        for (size_t index = first; index < pieceIndex; ++index) {
            const Piece& piece = pieces[index];
            const double pieceEnd = piece.start + piece.length;
            const double low = std::max(from, piece.start);
            const double high = std::min(to, pieceEnd);
            if (high <= low) {
                drawing = drawing && high == low && pieceEnd <= to;
                continue;
            }
            // Exact ends where the range covers them, so a trim that keeps
            // two neighbouring pieces keeps the join between them.
            const double t0 =
                low <= piece.start ? 0.0 : piece.ParameterAt(low - piece.start);
            const double t1 =
                high >= pieceEnd ? 1.0 : piece.ParameterAt(high - piece.start);
            if (!drawing || t0 > 0.0) {
                trimmed.push_back(MoveTo(piece.At(t0)));
            }
            if (piece.cubic) {
                Point c1;
                Point c2;
                Point p;
                SplitCubic(piece, t0, t1, c1, c2, p);
                trimmed.push_back(CubicTo(c1, c2, p));
            } else {
                trimmed.push_back(LineTo(piece.At(t1)));
            }
            drawing = t1 >= 1.0;
        }
    }
    return trimmed;
}

bool MotionPathBoundsOf(const MotionPath& path, MotionPathBounds& bounds) {
    bool any = false;
    Point current;
    Point origin;
    for (const MotionPathSegment& segment : path) {
        switch (segment.verb) {
            case MotionPathVerb::Move:
                current = origin = {segment.x, segment.y};
                break;
            case MotionPathVerb::Line:
                Include(bounds, any, current);
                current = {segment.x, segment.y};
                Include(bounds, any, current);
                break;
            case MotionPathVerb::Close:
                Include(bounds, any, current);
                Include(bounds, any, origin);
                current = origin;
                break;
            case MotionPathVerb::Cubic: {
                const Point p1{segment.x1, segment.y1};
                const Point p2{segment.x2, segment.y2};
                const Point p3{segment.x, segment.y};
                Include(bounds, any, current);
                Include(bounds, any, p3);
                for (double t : Extrema(current.x, p1.x, p2.x, p3.x)) {
                    Include(bounds, any, CubicPoint(current, p1, p2, p3, t));
                }
                for (double t : Extrema(current.y, p1.y, p2.y, p3.y)) {
                    Include(bounds, any, CubicPoint(current, p1, p2, p3, t));
                }
                current = p3;
                break;
            }
        }
    }
    return any;
}

void TransformMotionPath(MotionPath& path, double scale, double dx, double dy) {
    for (MotionPathSegment& segment : path) {
        segment.x1 = segment.x1 * scale + dx;
        segment.y1 = segment.y1 * scale + dy;
        segment.x2 = segment.x2 * scale + dx;
        segment.y2 = segment.y2 * scale + dy;
        segment.x = segment.x * scale + dx;
        segment.y = segment.y * scale + dy;
    }
}
