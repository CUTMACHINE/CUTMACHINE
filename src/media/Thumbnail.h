#pragma once

#include "MediaTaskManager.h"
#include "RationalTime.h"

#include <cstdint>
#include <string>

struct ThumbnailSettings {
    int32_t width = 320;
    int32_t height = 180;
    // Empty: the binary FfmpegBinary.h resolves (PRD1).
    std::string ffmpeg_path;
};

bool GenerateMediaThumbnail(const std::string& inputPath,
                            const std::string& outputPath,
                            const RationalTime& duration,
                            const ThumbnailSettings& settings,
                            MediaTaskContext& context, std::string& error);
