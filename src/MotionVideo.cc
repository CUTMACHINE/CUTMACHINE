#include "MotionVideo.h"

#include "Ulid.h"

#include <algorithm>
#include <filesystem>
#include <set>
#include <utility>

namespace {

using mcp_json::Value;

bool Fail(MotionError& error, std::string code, std::string path,
          std::string message) {
    error = {std::move(code), std::move(path), std::move(message)};
    return false;
}

bool Invalid(MotionError& error, std::string message) {
    return Fail(error, "InvalidExport", "output", std::move(message));
}

// The names to render: the ones asked for, checked once each, or every one
// declared, or the composition's own values (the empty name) when it
// declares none.
template <typename Set>
bool Names(const std::vector<std::string>& asked,
           const std::vector<Set>& declared, const char* field,
           std::vector<std::string>& names, bool& unnamed, MotionError& error) {
    names.clear();
    unnamed = false;
    if (asked.empty()) {
        for (const Set& set : declared) {
            names.push_back(set.name);
        }
        if (names.empty()) {
            names.emplace_back();
            unnamed = true;
        }
        return true;
    }
    for (const std::string& name : asked) {
        if (name.empty()) {
            return Fail(error, "InvalidValue", field,
                        std::string(field) + " holds an empty name");
        }
        if (std::find(names.begin(), names.end(), name) != names.end()) {
            return Fail(error, "InvalidValue", field,
                        std::string(field) + " names '" + name + "' twice");
        }
        names.push_back(name);
    }
    return true;
}

std::string Replaced(std::string text, const std::string& placeholder,
                     const std::string& value) {
    for (size_t at = text.find(placeholder); at != std::string::npos;
         at = text.find(placeholder, at + value.size())) {
        text.replace(at, placeholder.size(), value);
    }
    return text;
}

std::string Rate(const MotionComposition& composition) {
    return std::to_string(composition.fps_numerator) + "/" +
           std::to_string(composition.fps_denominator);
}

bool HasExtension(const std::filesystem::path& path,
                  std::initializer_list<const char*> extensions) {
    const std::string extension = path.extension().string();
    return std::any_of(
        extensions.begin(), extensions.end(),
        [&](const char* candidate) { return extension == candidate; });
}

bool CheckDestination(const MotionVideoItem& item,
                      const MotionVideoOptions& options,
                      std::set<std::filesystem::path>& destinations,
                      MotionError& error) {
    const std::filesystem::path destination(item.output_path);
    const std::string where = "'" + item.output_path + "'";
    if (destination.string().find_first_of("{}") != std::string::npos) {
        return Invalid(error, where +
                                  ": the output names only {variant} and "
                                  "{format}");
    }
    if (options.alpha
            ? !HasExtension(destination, {".mov", ".MOV"})
            : !HasExtension(destination, {".mov", ".MOV", ".mp4", ".MP4"})) {
        return Invalid(error,
                       where + (options.alpha ? ": ProRes 4444 with alpha is "
                                                "written to a .mov"
                                              : ": a motion video is a .mov or "
                                                "an .mp4"));
    }
    std::error_code status;
    const std::filesystem::path canonical =
        std::filesystem::weakly_canonical(destination, status);
    if (status || !destinations.insert(canonical).second) {
        return Invalid(error,
                       where +
                           " is written twice: put {variant} and {format} in "
                           "the output so each render has its own file");
    }
    if (!std::filesystem::is_directory(destination.parent_path(), status)) {
        return Invalid(error, where + ": its directory does not exist");
    }
    if (std::filesystem::exists(destination, status)) {
        if (!options.overwrite) {
            return Invalid(error, where + " already exists");
        }
        if (!std::filesystem::is_regular_file(destination, status)) {
            return Invalid(error, where + " exists and is not a file");
        }
    }
    const MotionComposition& sized = item.composition;
    if (!options.alpha && (sized.width % 2 != 0 || sized.height % 2 != 0)) {
        return Invalid(
            error, where + ": H.264 4:2:0 needs even dimensions, and " +
                       (item.format.empty() ? "the composition"
                                            : "format '" + item.format + "'") +
                       " is " + std::to_string(sized.width) + "x" +
                       std::to_string(sized.height) +
                       " (ProRes 4444, with alpha, takes any size)");
    }
    return true;
}

}  // namespace

bool ValidMotionLoudness(const MotionVideoOptions& options,
                         MotionError& error) {
    // Written so that a NaN is out of reason too.
    const auto within = [](double value, double low, double high) {
        return value >= low && value <= high;
    };
    if (options.loudness.has_value() &&
        !within(*options.loudness, -40.0, -5.0)) {
        error = {"InvalidValue", "loudness",
                 "loudness is an integrated loudness from -40 to -5 LUFS "
                 "(-16 for the web, -23 for broadcast)"};
        return false;
    }
    if (!within(options.true_peak, -9.0, 0.0)) {
        error = {"InvalidValue", "true_peak",
                 "true_peak is a ceiling from -9 to 0 dBTP (-1 by default)"};
        return false;
    }
    return true;
}

bool PlanMotionVideos(const MotionComposition& composition,
                      const MotionVideoOptions& options,
                      std::vector<MotionVideoItem>& items, MotionError& error) {
    items.clear();
    if (!ValidMotionLoudness(options, error)) {
        return false;
    }
    std::vector<std::string> variants;
    std::vector<std::string> formats;
    bool unnamedVariant = false;
    bool unnamedFormat = false;
    if (!Names(options.variants, composition.variants, "variants", variants,
               unnamedVariant, error) ||
        !Names(options.formats, composition.formats, "formats", formats,
               unnamedFormat, error)) {
        return false;
    }
    if (options.output.empty()) {
        return Invalid(error, "the output is empty");
    }
    if (unnamedVariant &&
        options.output.find("{variant}") != std::string::npos) {
        return Invalid(error,
                       "{variant} names nothing: the composition declares no "
                       "versions");
    }
    if (unnamedFormat && options.output.find("{format}") != std::string::npos) {
        return Invalid(error,
                       "{format} names nothing: the composition declares no "
                       "formats");
    }
    std::set<std::filesystem::path> destinations;
    std::vector<MotionVideoItem> planned;
    for (const std::string& variant : variants) {
        for (const std::string& format : formats) {
            MotionVideoItem item;
            item.variant = variant;
            item.format = format;
            if (!ResolveMotionVariant(composition, variant, format, Value(),
                                      item.composition, item.props, error)) {
                return false;
            }
            const std::string path =
                Replaced(Replaced(options.output, "{variant}", variant),
                         "{format}", format);
            std::error_code status;
            const std::filesystem::path absolute =
                std::filesystem::absolute(path, status);
            if (status) {
                return Invalid(error, "'" + path + "': " + status.message());
            }
            item.output_path = absolute.lexically_normal().string();
            if (!CheckDestination(item, options, destinations, error)) {
                return false;
            }
            planned.push_back(std::move(item));
        }
    }
    items = std::move(planned);
    return true;
}

ExportPlan MotionVideoExportPlan(const MotionVideoItem& item,
                                 const MotionVideoOptions& options,
                                 const std::string& framePattern,
                                 const std::string& audioPath) {
    const MotionComposition& composition = item.composition;
    const std::string rate = Rate(composition);
    ExportPlan plan;
    ExportSettings& settings = plan.settings;
    settings.output_path = item.output_path;
    settings.width = composition.width;
    settings.height = composition.height;
    settings.frame_rate = {composition.fps_numerator,
                           composition.fps_denominator};
    settings.overwrite = options.overwrite;
    settings.ffmpeg_path = options.ffmpeg_path;
    settings.main10 = false;
    if (!options.alpha) {
        const ExportSettings h264 = Exporter::H264Preset(item.output_path);
        settings.encoder = h264.encoder;
        settings.video_bitrate = h264.video_bitrate;
    }
    plan.duration = {composition.duration * composition.fps_denominator,
                     composition.fps_numerator};
    plan.total_frames = composition.duration;
    const std::filesystem::path destination(item.output_path);
    plan.temporary_output_path =
        (destination.parent_path() /
         (destination.stem().string() + ".cutmachine-" + GenerateUlid() +
          ".part" + destination.extension().string()))
            .string();
    // Frame counts, not timestamps: the sequence is read at the exact
    // cadence and exactly `duration` frames are written, as the timeline
    // export does since ITW1. Core Graphics draws sRGB; the conversion to
    // video names BT.709 both ways, since swscale would otherwise pick
    // BT.601 for an RGB input.
    plan.arguments = {"-hide_banner",  "-nostdin", "-y",         "-progress",
                      "pipe:1",        "-nostats", "-framerate", rate,
                      "-start_number", "0",        "-i",         framePattern};
    if (!audioPath.empty()) {
        plan.arguments.insert(plan.arguments.end(), {"-i", audioPath, "-map",
                                                     "0:v:0", "-map", "1:a:0"});
    }
    plan.arguments.insert(
        plan.arguments.end(),
        {"-vf", std::string("scale=out_color_matrix=bt709:out_range=tv,"
                            "format=") +
                    (options.alpha ? "yuva444p10le" : "yuv420p")});
    if (!audioPath.empty()) {
        const bool mov =
            std::filesystem::path(item.output_path).extension() == ".mov";
        plan.arguments.insert(
            plan.arguments.end(),
            {"-c:a", mov ? "pcm_s24le" : "aac", "-ar", "48000", "-ac", "2"});
        if (!mov) {
            plan.arguments.insert(plan.arguments.end(), {"-b:a", "320k"});
        }
    }
    if (options.alpha) {
        plan.arguments.insert(plan.arguments.end(),
                              {"-c:v", "prores_ks", "-profile:v", "4444",
                               "-vendor", "apl0", "-alpha_bits", "16"});
    } else {
        plan.arguments.insert(
            plan.arguments.end(),
            {"-c:v", "h264_videotoolbox", "-profile:v", "high", "-b:v",
             std::to_string(settings.video_bitrate), "-allow_sw", "1", "-tag:v",
             "avc1"});
    }
    plan.arguments.insert(
        plan.arguments.end(),
        {"-color_primaries", "bt709", "-color_trc", "bt709", "-colorspace",
         "bt709", "-color_range", "tv", "-movflags", "+faststart", "-r", rate,
         "-frames:v", std::to_string(plan.total_frames),
         plan.temporary_output_path});
    return plan;
}

Value MotionVideoItemReport(const MotionVideoItem& item,
                            const MotionVideoOptions& options,
                            const std::string& status, bool audio) {
    const MotionComposition& composition = item.composition;
    Value report = Value::MakeObject();
    report.Set("variant", item.variant.empty()
                              ? Value::MakeNull()
                              : Value::MakeString(item.variant));
    report.Set("format", item.format.empty() ? Value::MakeNull()
                                             : Value::MakeString(item.format));
    report.Set("path", Value::MakeString(item.output_path));
    report.Set("codec", Value::MakeString(options.alpha ? "prores" : "h264"));
    report.Set("profile", Value::MakeString(options.alpha ? "4444" : "high"));
    report.Set("alpha", Value::MakeBool(options.alpha));
    report.Set("width", Value::MakeInt(composition.width));
    report.Set("height", Value::MakeInt(composition.height));
    Value rate = Value::MakeObject();
    rate.Set("num", Value::MakeInt(composition.fps_numerator));
    rate.Set("den", Value::MakeInt(composition.fps_denominator));
    report.Set("frame_rate", std::move(rate));
    report.Set("frames", Value::MakeInt(composition.duration));
    // Written only when there is a sound, so that a silent card's report
    // keeps the bytes it had.
    if (audio) {
        report.Set(
            "audio",
            Value::MakeString(
                std::filesystem::path(item.output_path).extension() == ".mov"
                    ? "pcm_s24le"
                    : "aac"));
        // MM1 -- what the sound is brought to; the render adds what the
        // file measured.
        if (options.loudness.has_value()) {
            Value loudness = Value::MakeObject();
            loudness.Set("target_lufs", Value::MakeDouble(*options.loudness));
            loudness.Set("true_peak_ceiling_dbtp",
                         Value::MakeDouble(options.true_peak));
            report.Set("loudness", std::move(loudness));
        }
    }
    report.Set("status", Value::MakeString(status));
    return report;
}

std::string MotionVideoReport(const std::vector<Value>& outputs, bool ok,
                              bool preview, size_t total, size_t completed) {
    Value report = Value::MakeObject();
    report.Set("ok", Value::MakeBool(ok));
    report.Set("preview", Value::MakeBool(preview));
    report.Set("total", Value::MakeInt(static_cast<int64_t>(total)));
    report.Set("completed", Value::MakeInt(static_cast<int64_t>(completed)));
    Value list = Value::MakeArray();
    for (const Value& output : outputs) {
        list.Push(output);
    }
    report.Set("outputs", std::move(list));
    return report.Dump();
}
