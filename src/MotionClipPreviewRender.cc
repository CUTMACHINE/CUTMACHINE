#include "MotionClipPreviewRender.h"

#include "MotionDocument.h"
#include "MotionEvaluate.h"
#include "MotionRender.h"

bool RenderMotionPreview(const MotionPreviewRequest& request,
                         const Document& document,
                         const std::string& mediaDirectory,
                         MotionPreviewImage& image, MotionError& error) {
    if (request.composition == nullptr) {
        error = {"InvalidValue", "composition", "no composition to render"};
        return false;
    }
    MotionComposition composition;
    MotionComposition resolved;
    mcp_json::Value props;
    MotionCoreTextMeasurer measurer(mediaDirectory);
    MotionReach reach;
    MotionFrame frame;
    MotionRaster raster;
    if (!ParseDocumentComposition(*request.composition, composition, error) ||
        !MotionReachFor(document, request.composition->id, reach, error)) {
        return false;
    }
    measurer.SetMediaPaths(reach.media_paths);
    if (!ResolveMotionVariant(composition, std::string(), std::string(),
                              request.props, resolved, props, error) ||
        !EvaluateMotionFrame(resolved, props, request.frame, measurer,
                             reach.Context(), frame, error) ||
        !RenderMotionFrame(frame, measurer, raster, error)) {
        return false;
    }
    MotionPreviewFromPremultiplied(raster.pixels.data(), raster.width,
                                   raster.height, image);
    return true;
}
