#pragma once

// PA8 -- docs/roadmap/PARITE-2026-11.md. A clip's mask as every renderer
// evaluates it: the coverage at a point of the picture, written once here
// in C++, spelled once for FFmpeg (the export, read_frame, the sheets), and
// copied once in the monitor's shader (`clipMaskCoverage`, shader.metal),
// which tests/monitor_export_parity_tests.mm holds to the export.
//
// THE RULE. A point of the displayed source picture is (u, v), fractions of
// its width and height from its top left. With the mask's centre (cx, cy),
// half-width hw and half-height hh:
//
//     dx = (u - cx) / hw        dy = (v - cy) / hh
//     d  = hypot(dx, dy)        for an ellipse: 1 on its edge
//     d  = max(|dx|, |dy|)      for a rectangle: 1 on its edge
//     inside = clamp((1 - d) / feather, 0, 1)     with a feather
//     inside = d <= 1 ? 1 : 0                     without
//     coverage = inverted ? 1 - inside : inside
//
// The feather is measured on `d`, the distance to the centre in units of
// the shape's half-size: it eats inwards from the edge, by the same
// fraction on every side, so a wide rectangle fades over more pixels across
// than down. That is what one number can say of a shape that is not
// square; a feather in pixels would need the picture's aspect, and the
// mask would then change with the rush's rotation.
//
// WHERE IT IS APPLIED. On the picture alone, after its blur and before it
// is laid in the frame, as a multiplication of its alpha: the colours never
// see it. A pixel of the picture stands for the middle of its own square,
// (X + 1/2) / W of the region the picture shows; that region is not always
// the whole source -- a `fill` cuts it, a crop keeps part of it -- so every
// caller says which part of the displayed source its picture is
// (ClipMaskRegion), and the rule is evaluated on the source's own
// coordinates.

#include "Document.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <sstream>
#include <string>

// The coverage of `mask` at (u, v) of the displayed source: 1 where the
// picture is kept, 0 where it is cut out. 1 everywhere for no mask.
inline double ClipMaskCoverage(const ClipMask& mask, double u, double v) {
    // PA15 -- a mask of the people is a matte, not a rule (PersonMatte.h).
    if (mask.shape.empty() || mask.shape == kClipMaskPerson) return 1.0;
    const auto fraction = [](const EffectParamValue& value) {
        return static_cast<double>(value.num) / static_cast<double>(value.den);
    };
    const double dx =
        (u - fraction(mask.center_x)) / (fraction(mask.width) / 2.0);
    const double dy =
        (v - fraction(mask.center_y)) / (fraction(mask.height) / 2.0);
    const double distance = mask.shape == kClipMaskEllipse
                                ? std::hypot(dx, dy)
                                : std::max(std::fabs(dx), std::fabs(dy));
    const double feather = fraction(mask.feather);
    const double inside = feather > 0.0
                              ? std::clamp((1.0 - distance) / feather, 0.0, 1.0)
                              : (distance <= 1.0 ? 1.0 : 0.0);
    return mask.inverted ? 1.0 - inside : inside;
}

// The part of the displayed source a picture shows, in displayed source
// pixels: what a crop kept of it, or the whole of it.
struct ClipMaskRegion {
    int64_t x = 0;
    int64_t y = 0;
    int64_t width = 0;
    int64_t height = 0;
    // The displayed source's own size.
    int64_t source_width = 0;
    int64_t source_height = 0;
};

// The FFmpeg filters that multiply a picture's alpha by `mask`, to put in
// a chain after a format that carries an alpha: the alpha is taken out as
// a grey plane, multiplied point by point (`geq`, the rule above in its
// expression language, fractions spelled exactly) and put back. Empty for
// no mask, and for a mask of the people (PersonMatte.h, MatteFilter). `label`
// names the three pads the split needs; it must be unique in the graph.
inline std::string ClipMaskFilter(const ClipMask& mask,
                                  const ClipMaskRegion& region,
                                  const std::string& label) {
    if (mask.shape.empty() || mask.shape == kClipMaskPerson ||
        region.width <= 0 || region.height <= 0 || region.source_width <= 0 ||
        region.source_height <= 0)
        return std::string();
    const auto fraction = [](const EffectParamValue& value) {
        return "(" + std::to_string(value.num) + "/" +
               std::to_string(value.den) + ")";
    };
    std::ostringstream expression;
    expression.imbue(std::locale::classic());
    // (u - cx) / hw, u the middle of pixel X carried into the source.
    const auto axis = [&](const char* pixel, const char* size, int64_t start,
                          int64_t span, int64_t source,
                          const EffectParamValue& centre,
                          const EffectParamValue& extent) {
        std::ostringstream text;
        text.imbue(std::locale::classic());
        text << "(((" << start << "+(" << pixel << "+0.5)*" << span << '/'
             << size << ")/" << source << '-' << fraction(centre) << ")/("
             << fraction(extent) << "/2))";
        return text.str();
    };
    const std::string dx = axis("X", "W", region.x, region.width,
                                region.source_width, mask.center_x, mask.width);
    const std::string dy =
        axis("Y", "H", region.y, region.height, region.source_height,
             mask.center_y, mask.height);
    const std::string distance = mask.shape == kClipMaskEllipse
                                     ? "hypot(" + dx + "," + dy + ")"
                                     : "max(abs(" + dx + "),abs(" + dy + "))";
    std::string inside;
    if (mask.feather.num > 0) {
        inside =
            "clip((1-" + distance + ")/" + fraction(mask.feather) + ",0,1)";
    } else {
        inside = "lte(" + distance + ",1)";
    }
    if (mask.inverted) inside = "(1-" + inside + ")";
    std::ostringstream filter;
    filter.imbue(std::locale::classic());
    filter << "split[mka" << label << "][mkb" << label << "];[mkb" << label
           << "]alphaextract,geq=lum='lum(X,Y)*" << inside << "'[mkm" << label
           << "];[mka" << label << "][mkm" << label << "]alphamerge";
    return filter.str();
}
