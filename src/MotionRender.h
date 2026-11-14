#pragma once

// MOTION-2026-09 M2 -- ROADMAP.md. The macOS side of motion: Core Text behind
// the MotionMeasurer seam, and Core Graphics rasterising an evaluated frame.
// Layout policy stays in MotionEvaluate (portable); this file only measures
// glyphs and draws what the evaluated tree already decided. Nothing here
// exposes a Core Text or Core Graphics type, so callers stay plain C++.

#include "MotionEvaluate.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

// Makes every .ttf, .otf and .ttc in `directory` available to this process
// only. A composition's fonts can then travel with it instead of depending on
// what happens to be installed; when the same face is also installed, the
// registered file wins.
bool RegisterMotionFontDirectory(const std::string& directory,
                                 MotionError& error);

// AX-2026-09 AX6 -- the families Core Text offers this process, under the
// name a composition gives them (kCTFontFamilyNameAttribute, which layout
// matches), registered directories included; hidden system families too.
// In Core Text's order, each once (FontCatalog.h sorts and filters).
bool ListMotionFontFamilies(std::vector<std::string>& families,
                            MotionError& error);

class MotionCoreTextMeasurer final : public MotionMeasurer {
public:
    // Relative image sources resolve against `media_directory`.
    explicit MotionCoreTextMeasurer(std::string media_directory = {});
    ~MotionCoreTextMeasurer() override;

    // REMOTION-PARITY-2026-09 REM2 -- the project's media, ULID to path
    // (relative paths resolve against the media directory): what a video or
    // an audio node that names `media` plays.
    void SetMediaPaths(std::map<std::string, std::string> paths);

    // PERF11 -- docs/roadmap/PERF-2026-09.md. When frames are rendered at
    // the same time, the turn in which the frame painted with this measurer
    // decodes its video pictures: `ordinal` is its rank among them.
    // RenderMotionFrame enters the turn if the frame shows a video and
    // always leaves it. Null (the default): no order, as when frames are
    // rendered one after the other.
    void SetMediaTurns(MotionMediaTurns* turns, size_t ordinal);

    MotionCoreTextMeasurer(const MotionCoreTextMeasurer&) = delete;
    MotionCoreTextMeasurer& operator=(const MotionCoreTextMeasurer&) = delete;

    bool LayoutText(const std::string& text, const MotionTextStyle& style,
                    double max_width, MotionTextLayout& layout,
                    MotionError& error) override;

    bool ImageSize(const std::string& source, double& width, double& height,
                   MotionError& error) override;

    bool MediaInfo(const std::string& src, const std::string& media,
                   MotionMediaInfo& info, MotionError& error) override;

    bool MediaSound(const std::string& src, const std::string& media,
                    std::shared_ptr<const MotionPcm>& pcm,
                    MotionError& error) override;

    // The font file a style resolves to, for diagnosing a rendering that does
    // not match: empty for the system UI font.
    bool ResolveFontFile(const MotionTextStyle& style, std::string& file,
                         MotionError& error);

    // REM3 -- the PostScript name of the face a style resolves to: which of
    // a family's faces was taken, its italic included.
    bool ResolveFontName(const MotionTextStyle& style, std::string& name,
                         MotionError& error);

    bool FontPostScriptName(const MotionTextStyle& style, std::string& name,
                            MotionError& error) override {
        return ResolveFontName(style, name, error);
    }

    // MOTION-WEB-2026-10 MW7 -- the weight a variable face is set to on its
    // wght axis, and whether the font names an instance there.
    bool FontVariableWeight(const MotionTextStyle& style, int& weight,
                            bool& named) override;

    // MM2 -- the text laid out with and without the variant, glyph by
    // glyph and advance by advance.
    bool NumericVariantChanges(const std::string& text,
                               const MotionTextStyle& style) override;

    struct Impl;
    Impl& impl() { return *impl_; }

private:
    std::unique_ptr<Impl> impl_;
};

// Premultiplied RGBA, 8 bits per channel, sRGB, rows from the top.
struct MotionRaster {
    int32_t width = 0;
    int32_t height = 0;
    std::vector<uint8_t> pixels;
};

// Draws an evaluated frame with the fonts and images the same measurer
// resolved during layout, so glyphs land exactly where the boxes say.
bool RenderMotionFrame(const MotionFrame& frame,
                       MotionCoreTextMeasurer& measurer, MotionRaster& raster,
                       MotionError& error);

// Refuses to overwrite: numbered frames are what make comparisons possible.
bool WriteMotionPng(const MotionRaster& raster, const std::string& path,
                    MotionError& error);

// MOTION-2026-09 M3 -- the same PNG bytes, in memory.
bool EncodeMotionPng(const MotionRaster& raster, std::string& png,
                     MotionError& error);
