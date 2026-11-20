#include "Export.h"

#include "AudioMixFilters.h"
#include "CaptionGeneration.h"
#include "ClipAnimation.h"
#include "ClipBlur.h"
#include "ClipEnabled.h"
#include "ClipFraming.h"
#include "ClipMask.h"
#include "ClipSpeed.h"
#include "ColorEffects.h"
#include "ColorManagement.h"
#include "FfmpegCapabilities.h"
#include "FfmpegPipe.h"
#include "Ingest.h"
#include "Json.h"
#include "LookLutStorage.h"
#include "MotionAudio.h"
#include "MotionClip.h"
#include "MotionDocument.h"
#include "MotionFrameRenderer.h"
#include "MotionParallelRender.h"
#include "PersonMatte.h"
#include "SourceGrid.h"
#include "Timeline.h"
#include "TimelineAudioReport.h"
#include "TransitionRender.h"
#include "Ulid.h"

extern "C" {
#include <libavutil/parseutils.h>
#include <libavutil/pixdesc.h>
}

#include <unistd.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <set>
#include <sstream>
#include <string_view>
#include <utility>

namespace {

std::string Decimal(const RationalTime& time) {
    return audio_mix::Seconds(time);
}

// ITW1 -- ROADMAP.md. FFmpeg evaluates filter expressions in double precision
// and truncates when it stores a timestamp: `(232/25)/TB` came out as
// 231.99999999999997, so a shot started on the next source frame and later
// repeated one to catch up, and `lt(t, in+duration)` kept 133 positions in
// 1400 on screen one frame too long at 25 fps. Placement is therefore an
// integer frame count at the export cadence, and nothing divides.
int64_t CeilFrames(const RationalTime& time, MediaRate rate) {
    const __int128 numerator = static_cast<__int128>(time.value) * rate.num;
    const __int128 denominator = static_cast<__int128>(time.rate) * rate.den;
    __int128 frames = numerator / denominator;
    if (numerator % denominator != 0 && numerator > 0) ++frames;
    if (frames > std::numeric_limits<int64_t>::max() ||
        frames < std::numeric_limits<int64_t>::min()) {
        throw std::overflow_error("export frame index overflows int64_t");
    }
    return static_cast<int64_t>(frames);
}

// The expression variable `t` is `frame * TB` in double. A bound half a frame
// before a frame's start sits a whole 1/(2*rate) away from every frame time,
// which no rounding error can cross: frames [first, end) and only those pass.
std::string FrameWindow(int64_t first, int64_t end, MediaRate rate) {
    const auto halfFrameBefore = [&](int64_t frame) {
        return "((" + std::to_string((2 * frame) - 1) + '*' +
               std::to_string(rate.den) + ")/" + std::to_string(2 * rate.num) +
               ')';
    };
    return "gte(t\\," + halfFrameBefore(first) + ")*lt(t\\," +
           halfFrameBefore(end) + ')';
}

// F1.3 -- share one temporary LUT for identical GPU-resolved stacks; effect
// IDs do not change the rendered color, but stack order and amounts do.
std::string ColorGradeKey(const ResolvedColorGrade& grade) {
    std::ostringstream key;
    key.imbue(std::locale::classic());
    key << grade.count;
    for (int32_t index = 0; index < grade.count; ++index) {
        key << ':' << grade.entries[index].kind << ':' << std::hexfloat
            << grade.entries[index].amount;
        // Wheels and curves carry their knobs in the data pool.
        const ResolvedColorEffect& entry = grade.entries[index];
        for (int32_t at = 0; at < entry.data_count; ++at) {
            key << ',' << grade.data.at(entry.data_offset + at);
        }
    }
    return key.str();
}

}  // namespace

// F1.3 -- an RGB picture has no YCbCr matrix or range of its own, so the
// matrix and range the settings declare for the camera files must not be
// applied to it: the managed graph packs it once as full-range BT.709 YCbCr
// and reads it back the same way.
bool IsRgbPixelFormat(const std::string& name) {
    const AVPixelFormat format = av_get_pix_fmt(name.c_str());
    if (format == AV_PIX_FMT_NONE) {
        return false;
    }
    const AVPixFmtDescriptor* descriptor = av_pix_fmt_desc_get(format);
    return descriptor != nullptr &&
           (descriptor->flags & (AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_PAL)) !=
               0;
}

int PixelFormatBitDepth(const std::string& name) {
    const AVPixelFormat format = av_get_pix_fmt(name.c_str());
    if (format == AV_PIX_FMT_NONE) return 0;
    const AVPixFmtDescriptor* descriptor = av_pix_fmt_desc_get(format);
    return descriptor != nullptr ? descriptor->comp[0].depth : 0;
}

namespace {

// R5 -- docs/roadmap/REEL-2026-09.md. One side of a colour dip: a ramp of
// `frames` frames starting at sequence frame `start`, towards the colour
// when `to_color` and away from it otherwise. Exactly the two branches of
// the curve TransitionRender.h writes -- k/pre before the cut, (n-k)/post
// after it -- which is why nothing here recomputes one: FFmpeg's `fade` is
// the same ramp, over `n` frames from `s`. The engine hands it the two
// integers it derived and no expression, so the frame number stays the only
// time variable in the graph (guard rail 2).
struct ExportColorFade {
    int64_t start = 0;
    int64_t frames = 0;
    bool to_color = false;
    TransitionColor color;
    // The veil's exact strength at each of those frames, as ColorVeilAlpha
    // computes it: what the managed chain hands FFmpeg frame by frame, where
    // `fade` cannot work in float (it runs in 16 bits, and swscale reads
    // 65535 back as 0.9961).
    std::vector<EffectParamValue> alphas;
};

// VQA2 -- docs/roadmap/QA-AGENTIQUE-2026-09.md. The dip is a veil laid over
// the picture, as the monitor draws it: a flat layer of the colour whose
// ALPHA follows the ramp, sized to the picture by `scale2ref` and put on
// with `overlay`. `fade` used to act on the picture itself. On the QA run's
// saturated test pattern, that ramp was not the monitor's: an opening from
// black showed 15 % of the shot at frame 10 of 40 in the file and 25 % in
// Record, the gap closing only at the end. Planar RGB above 8 bits gave the
// same curve, 8-bit RGB the right one; a flat grey showed none of it, which
// is why the exact-luma test below never saw it. The veil matches Record at
// every position measured, and the picture keeps its own depth. The layer
// becomes BT.709 limited range before it meets the picture, as a motion clip
// does.
//
// `localStart` is the dip's first frame counted in the chain's own frames;
// `label` names this veil in the graph.
void AppendColorVeil(std::ostringstream& chains, std::ostringstream& graph,
                     const std::string& label, const std::string& rate,
                     const RationalTime& duration, int64_t localStart,
                     int64_t frames, bool toColor,
                     const TransitionColor& color) {
    chains << "color=c=0x" << TransitionColorHex(color) << ":s=16x16:r=" << rate
           << ":d=" << Decimal(duration)
           << ",format=gbrap,fade=t=" << (toColor ? "in" : "out")
           << ":s=" << localStart << ":n=" << frames << ":alpha=1[" << label
           << "src];";
    graph << ",format=yuv444p10le,setpts=PTS-STARTPTS[" << label << "under];["
          << label << "src][" << label
          << "under]scale2ref=w=rw:h=rh:flags=neighbor[" << label << "sized]["
          << label << "picture];[" << label << "sized]scale=out_color_matrix="
          << "bt709:out_range=tv,format=yuva444p10le[" << label << "];["
          << label << "picture][" << label
          << "]overlay=format=yuv444p10:eof_action=pass:shortest=0";
}

struct ManagedLayer {
    int input_index = -1;
    size_t ordinal = 0;
    int64_t first_frame = 0;
    int64_t frame_count = 0;
    bool fade = false;
    int64_t fade_frames = 0;
    EffectParamValue opacity{1, 1};
    bool rgb_source = false;
    // PA8 -- docs/roadmap/PARITE-2026-11.md. The clip's mask as filters
    // (ClipMaskFilter), put on the picture alone before it is padded into
    // the frame; empty for a clip without one, and for a prepositioned
    // layer, whose chain masked it before placing it.
    std::string mask_filter;
    std::string cube_path;
    // Visual QA of 19 September 2026 -- a display-referred layer (a motion
    // clip) is mixed in the SDR display view, not in the working space
    // (OpenColorIoCompositeDisplayReferred): the composite under it goes out
    // through `view_shaper_path` (empty when not grading in ACEScct) and
    // `view_cube_path`, and the mix comes back through `cube_path`.
    bool display_referred = false;
    std::string view_shaper_path;
    std::string view_cube_path;
    // The unit cube (UnitCubeText) that makes the coverage mask exact.
    std::string coverage_cube_path;
    // R1 -- where ClipFraming.h put this clip's picture in the frame.
    bool framed = false;
    int64_t framing_x = 0;
    int64_t framing_y = 0;
    // R2 -- docs/roadmap/REEL-2026-09.md. The chain already holds a
    // frame-sized picture whose alpha is its coverage: an animated clip is
    // placed by `overlay` on a transparent canvas, at the per-frame list
    // ClipAnimation.h sampled, so there is nothing left for `pad` to place.
    bool prepositioned = false;
    // R2 -- the clip's opacity at each of its output frames, when it moves;
    // empty otherwise, and `opacity` above is then the constant.
    std::vector<EffectParamValue> opacity_by_frame;
    // R5 -- docs/roadmap/REEL-2026-09.md. The colour dips over this clip,
    // in the clip's own frames, and what laying them in the SDR view takes:
    // the view (shaper and cube, as for a motion layer) and its inverse.
    std::vector<ExportColorFade> veils;
    std::string veil_view_shaper_path;
    std::string veil_view_cube_path;
    std::string veil_inverse_cube_path;
};

// R5 -- docs/roadmap/REEL-2026-09.md. A colour's code value as the exact
// float colorlevels is handed: FFmpeg's `color` source converted to float
// by swscale reads 255 as 0.9962, which is a colour the author did not
// write.
std::string VeilLevels(const TransitionColor& color) {
    std::ostringstream levels;
    levels.imbue(std::locale::classic());
    levels.precision(9);
    const auto channel = [&](char name, int value) {
        const double level = value / 255.0;
        levels << name << "omin=" << level << ':' << name << "omax=" << level;
    };
    channel('r', color.red);
    levels << ':';
    channel('g', color.green);
    levels << ':';
    channel('b', color.blue);
    return levels.str();
}

// Visual QA of 19 September 2026 -- the identity on [0, 1] as a two-point
// cube: lut3d clamps its input to the cube, so a value at or above 1 comes
// out as the corner's 1 exactly, and 0 as 0. swscale cannot give that: a
// mask built in 16 bits read back as 0.9961 (65280/65535), and the 0.4 %
// left of a highlight under a title showed through it
// (cutmachine_monitor_export_parity_tests).
std::string UnitCubeText() {
    return "TITLE \"CUTMACHINE unit cube\"\nLUT_3D_SIZE 2\n"
           "0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n";
}

// R2 -- docs/roadmap/REEL-2026-09.md. An animated parameter as an explicit
// per-frame list, which is the construction R3 announced for a ramp and the
// reason it announced it: `fps` resamples a constant cadence and `scale`
// takes a size, neither takes a curve, and an expression FFmpeg evaluates
// itself would be a second answer to a question ClipAnimation.h has already
// answered (guard rail 1). So the engine samples every frame and the graph
// only spells the values out.
//
// Runs of equal values collapse to one leaf, so a hold of a hundred frames
// is one value and not a hundred, and the leaves are reached by a binary
// search on the frame number: `if(lt(n-1,first),left,right)`. A flat sum of
// terms, the first spelling, stopped parsing at 99 of them -- measured on
// FFmpeg 8.1.2, "Cannot parse expression" -- so a zoom that moved on more
// than four seconds of a real rush refused to export. The tree is log2 deep
// (eleven levels for a minute at 25 fps), and FFmpeg's `if` evaluates only
// the branch it takes. The two outer leaves are open-ended, so that a frame
// outside the list -- a transition handle, a repeated last frame -- reads
// the nearest value instead of falling through to zero, which `scale` would
// take as "keep the original size".
//
// THE INDEX IS `n - 1`, AND THAT IS MEASURED, NOT DEDUCED. On FFmpeg 8.1.2,
// both `scale` with `eval=frame` and `overlay` evaluate their expressions
// with `n` already incremented: the first frame sees n = 1. Measured with a
// white square scaled and placed by an explicit list: with `n` the sizes and
// the offsets came out one frame early and the last frame fell off the end
// of the list; with `n - 1` every frame landed on its own value. `geq`, by
// contrast, counts from zero, and the alpha list below uses `N` unshifted.
std::string FrameValueList(const std::vector<int64_t>& values,
                           int64_t firstIndex, const std::string& variable) {
    if (values.empty()) return "0";
    struct Run {
        int64_t value = 0;
        int64_t first = 0;
    };
    std::vector<Run> runs;
    for (size_t index = 0; index < values.size(); ++index) {
        const int64_t at = firstIndex + static_cast<int64_t>(index);
        if (!runs.empty() && runs.back().value == values[index]) continue;
        runs.push_back({values[index], at});
    }
    std::ostringstream list;
    list.imbue(std::locale::classic());
    // Runs [low, high], by halves: the right half starts at runs[middle].
    const std::function<void(size_t, size_t)> spell = [&](size_t low,
                                                          size_t high) {
        if (low == high) {
            list << runs[low].value;
            return;
        }
        const size_t middle = low + ((high - low + 1) / 2);
        list << "if(lt(" << variable << ',' << runs[middle].first << "),";
        spell(low, middle - 1);
        list << ',';
        spell(middle, high);
        list << ')';
    };
    spell(0, runs.size() - 1);
    return list.str();
}

// R3, criterion 4 -- docs/roadmap/REEL-2026-09.md. FrameValueList's
// construction for a list that climbs: the values over consecutive indices
// from zero, cut into runs that rise by one constant step, each run spelled
// `value + (variable - first) * step`. A ramp's frame times are such runs
// wherever its speed holds -- one run for a whole stretch at 1/1 or at 1/4
// -- and a list one entry per frame would spell a five-second shot as 125
// nested tests. Integers only, so FFmpeg's double evaluates every one of
// them exactly.
std::string FrameProgressionList(const std::vector<int64_t>& values,
                                 const std::string& variable) {
    if (values.empty()) return "0";
    struct Run {
        int64_t first = 0;
        int64_t value = 0;
        int64_t step = 0;
    };
    std::vector<Run> runs;
    for (size_t index = 0; index < values.size();) {
        Run run{static_cast<int64_t>(index), values[index], 0};
        size_t end = index + 1;
        if (end < values.size()) {
            run.step = values[end] - values[index];
            while (end + 1 < values.size() &&
                   values[end + 1] - values[end] == run.step) {
                ++end;
            }
            ++end;
        }
        runs.push_back(run);
        index = end;
    }
    std::ostringstream list;
    list.imbue(std::locale::classic());
    const std::function<void(size_t, size_t)> spell = [&](size_t low,
                                                          size_t high) {
        if (low == high) {
            const Run& run = runs[low];
            list << run.value;
            if (run.step != 0) {
                list << "+(" << variable << '-' << run.first << ")*"
                     << run.step;
            }
            return;
        }
        const size_t middle = low + ((high - low + 1) / 2);
        list << "if(lt(" << variable << ',' << runs[middle].first << "),";
        spell(low, middle - 1);
        list << ',';
        spell(middle, high);
        list << ')';
    };
    spell(0, runs.size() - 1);
    return list.str();
}

// F1.3 -- ROADMAP.md, linear composite (18 September 2026). Continues one
// colour-managed clip after its fps/scale/setsar prefix and composites it
// over base<ordinal>, in float and in scene-linear light, as the monitor's
// working texture does. The overlay filter only blends 8- and 10-bit
// formats, so the clip's coverage travels in alpha instead: pad marks the
// letterbox as transparent, fade and opacity scale it, and maskedmerge uses
// it as the mask. Unlike overlay, maskedmerge drops every base frame until
// its other inputs have one, so a clip that starts later is led by a single
// transparent frame at zero.
void AppendManagedLayer(std::ostringstream& graph, const ManagedLayer& layer,
                        const ColorManagementSettings& color,
                        const ExportSettings& settings) {
    if (layer.prepositioned) {
        graph << ",format=yuva444p16le";
    } else {
        if (layer.rgb_source) {
            graph << ",scale=out_range=full:out_color_matrix=bt709";
        }
        graph << ",format=yuva444p16le";
        if (!layer.mask_filter.empty()) graph << ',' << layer.mask_filter;
        graph << ",pad=" << settings.width << ':' << settings.height << ':';
        // R1 -- docs/roadmap/REEL-2026-09.md. The managed composite places
        // its layer with `pad` where the unmanaged one uses `overlay`; a
        // framed clip is padded to the rectangle ClipFraming.h named, an
        // unframed one keeps the centring expression it had.
        if (layer.framed) {
            graph << layer.framing_x << ':' << layer.framing_y;
        } else {
            graph << "(ow-iw)/2:(oh-ih)/2";
        }
        graph << ":color=black@0";
    }
    const size_t ordinal = layer.ordinal;
    // R5 -- docs/roadmap/REEL-2026-09.md. A dip covers the picture's
    // rectangle, as the monitor paints it over the rectangle it has just
    // drawn: here that rectangle is the layer's coverage, read before any
    // fade or opacity scales it, so no second answer to where the picture
    // lies is ever computed (guard rail 1). Each dip takes a copy.
    if (!layer.veils.empty()) {
        graph << ",split=" << (layer.veils.size() + 1) << "[lay" << ordinal
              << ']';
        for (size_t index = 0; index < layer.veils.size(); ++index) {
            graph << "[vr" << ordinal << '_' << index << ']';
        }
        graph << ";[lay" << ordinal << "]null";
    }
    const bool led = layer.first_frame > 0;
    if (led) {
        graph << ",tpad=start=1:start_mode=add:color=black@0";
    }
    if (layer.fade) {
        graph << ",fade=t=in:s=" << (led ? 1 : 0) << ":n=" << layer.fade_frames
              << ":alpha=1";
    }
    graph << ",scale="
          << (layer.rgb_source ? "in_range=full:in_color_matrix=bt709:"
                               : ColorInputScaleOptions(color))
          << "out_range=full,format=gbrapf32le";
    if (!layer.display_referred) {
        graph << ",lut3d=file='" << layer.cube_path << "':interp=trilinear";
    }
    if (!layer.opacity_by_frame.empty()) {
        // R2 -- docs/roadmap/REEL-2026-09.md. An animated opacity, as the
        // unmanaged chain hands it over: `sendcmd` gives colorchannelmixer
        // the frame's value as that frame arrives. The alpha here is float,
        // so the value goes as the exact fraction the engine sampled, not
        // as the 8-bit code the unmanaged alpha carries. The chain's frame
        // zero is the leading transparent frame when there is one.
        std::ostringstream commands;
        commands.imbue(std::locale::classic());
        const int64_t lead = led ? 1 : 0;
        EffectParamValue previous{-1, 1};
        for (size_t frame = 0; frame < layer.opacity_by_frame.size(); ++frame) {
            const EffectParamValue& value = layer.opacity_by_frame[frame];
            if (frame > 0 && value.num == previous.num &&
                value.den == previous.den) {
                continue;
            }
            previous = value;
            const RationalTime at{
                (static_cast<int64_t>(frame) + lead) * settings.frame_rate.den,
                settings.frame_rate.num};
            commands << (commands.tellp() > 0 ? ";" : "") << Decimal(at)
                     << " colorchannelmixer@opacity" << ordinal << " aa ("
                     << value.num << '/' << value.den << ')';
        }
        graph << ",setpts=PTS-STARTPTS,sendcmd=c='" << commands.str()
              << "',colorchannelmixer@opacity" << ordinal << "=aa=1";
    } else if (layer.opacity.num != layer.opacity.den) {
        graph << ",colorchannelmixer=aa=(" << layer.opacity.num << '/'
              << layer.opacity.den << ')';
    }
    // Frame counts, not timestamps, as for the unmanaged placement: frame
    // zero is the leading transparent frame, frame n the clip's frame n-1.
    const auto placement = [&](std::ostringstream& chain) {
        if (led) {
            chain << ",setpts=if(eq(N\\,0)\\,0\\,N-1+" << layer.first_frame
                  << ')';
        } else {
            chain << ",setpts=PTS-STARTPTS+" << layer.first_frame;
        }
    };
    placement(graph);
    const std::string window =
        FrameWindow(layer.first_frame, layer.first_frame + layer.frame_count,
                    settings.frame_rate);
    // R5 -- the dips over this layer, laid once the layer is in the
    // composite: the veil belongs to the track and covers what was just
    // drawn, as in the monitor (Renderer.mm paints it right after the
    // layer). A dip's colour is a graphic colour, so it is mixed where
    // graphic colours are authored, by the rule a motion layer follows
    // (OpenColorIoCompositeDisplayReferred): the composite goes out through
    // the SDR view, the colour is mixed there by the veil's strength, and
    // the mix comes back through the view's inverse. Laid anywhere else --
    // in the camera's encoding, measured 0.26 off -- a white dip is not
    // white. For an HLG delivery that view is the HLG one (BH1): a white
    // dip reaches the HLG peak, as a white title does.
    //
    // The veil's strength is the layer's exact coverage (the unit cube, as
    // for [k] below) times the strength ColorVeilAlpha gave each frame:
    // k/pre towards the colour, (n-k)/post away from it, the curve the
    // unmanaged veil draws with `fade`. The colour is derived from the same
    // stream by colorlevels, so it has the veil's frames and nothing to
    // synchronise.
    // [vk] marks every pixel the veil reaches; elsewhere the composite is
    // kept bit for bit. The view transforms only run inside the dip.
    const auto appendVeils = [&](size_t firstBase) {
        for (size_t index = 0; index < layer.veils.size(); ++index) {
            const ExportColorFade& veil = layer.veils[index];
            const std::string id =
                std::to_string(ordinal) + '_' + std::to_string(index);
            const size_t below = firstBase + index;
            const std::string dipWindow =
                FrameWindow(layer.first_frame + veil.start,
                            layer.first_frame + veil.start + veil.frames,
                            settings.frame_rate);
            graph << "[vr" << id << ']';
            if (led) {
                graph << "tpad=start=1:start_mode=add:color=black@0,";
            }
            // The strength, frame by frame, as the engine computed it:
            // `sendcmd` sets colorlevels' output ceiling, which scales the
            // exact 0/1 coverage into coverage times strength, in float.
            std::ostringstream ramp;
            ramp.imbue(std::locale::classic());
            const std::string filter = "colorlevels@veil" + id;
            EffectParamValue current{-1, 1};
            for (size_t frame = 0; frame < veil.alphas.size(); ++frame) {
                const EffectParamValue& alpha = veil.alphas[frame];
                if (alpha.num == current.num && alpha.den == current.den) {
                    continue;
                }
                current = alpha;
                const RationalTime at{
                    (veil.start + static_cast<int64_t>(frame) + (led ? 1 : 0)) *
                        settings.frame_rate.den,
                    settings.frame_rate.num};
                const std::string value = "(" + std::to_string(alpha.num) +
                                          '/' + std::to_string(alpha.den) + ')';
                ramp << (ramp.tellp() > 0 ? ";" : "") << Decimal(at) << ' '
                     << filter << " romax " << value << ',' << filter
                     << " gomax " << value << ',' << filter << " bomax "
                     << value;
            }
            const EffectParamValue first = veil.alphas.empty()
                                               ? EffectParamValue{0, 1}
                                               : veil.alphas.front();
            const std::string initial = "(" + std::to_string(first.num) + '/' +
                                        std::to_string(first.den) + ')';
            graph << "format=gbrapf32le,colorchannelmixer=rr=0:ra=1:gg=0:"
                     "ga=1:bb=0:ba=1,colorlevels=rimax=0.00001:gimax="
                     "0.00001:bimax=0.00001,lut3d=file='"
                  << layer.coverage_cube_path
                  << "':interp=trilinear,setpts=PTS-STARTPTS,sendcmd=c='"
                  << ramp.str() << "'," << filter << "=romax=" << initial
                  << ":gomax=" << initial << ":bomax=" << initial;
            placement(graph);
            graph << ",split=3[vm" << id << "][vn" << id << "][vo" << id
                  << "];[vo" << id << "]colorlevels=" << VeilLevels(veil.color)
                  << "[vc" << id << "];[vn" << id
                  << "]colorlevels=rimax=0.00001:gimax=0.00001:bimax=0.00001,"
                     "lut3d=file='"
                  << layer.coverage_cube_path << "':interp=trilinear[vk" << id
                  << "];[base" << below << "]split[vb" << id << "][vv" << id
                  << "];[vv" << id << ']';
            if (!layer.veil_view_shaper_path.empty()) {
                graph << "lut1d=file='" << layer.veil_view_shaper_path
                      << "':interp=linear:enable=" << dipWindow << ',';
            }
            graph << "lut3d=file='" << layer.veil_view_cube_path
                  << "':interp=trilinear:enable=" << dipWindow
                  << ",lut3d=file='" << layer.coverage_cube_path
                  << "':interp=trilinear:enable=" << dipWindow << "[vd" << id
                  << "];[vd" << id << "][vc" << id << "][vm" << id
                  << "]maskedmerge=planes=7:enable=" << dipWindow
                  << ",lut3d=file='" << layer.veil_inverse_cube_path
                  << "':interp=trilinear:enable=" << dipWindow << "[vw" << id
                  << "];[vb" << id << "][vw" << id << "][vk" << id
                  << "]maskedmerge=planes=7:enable=" << dipWindow << "[base"
                  << (below + 1) << "];";
        }
    };
    if (!layer.display_referred) {
        graph << ",split[c" << ordinal << "][m" << ordinal << "];[m" << ordinal
              << "]colorchannelmixer=rr=0:ra=1:gg=0:ga=1:bb=0:ba=1[a" << ordinal
              << "];[base" << ordinal << "][c" << ordinal << "][a" << ordinal
              << "]maskedmerge=planes=7:enable=" << window << "[base"
              << (ordinal + 1) << "];";
        appendVeils(ordinal + 1);
        return;
    }
    // [c] holds the layer's display code values and [a] its coverage in
    // every colour plane. The composite goes out through the SDR view [d],
    // bounded to [0, 1] by its cube (BuildOpenColorIoDisplayCubes), the
    // layer is mixed into it there, and the mix comes back to the working
    // space [w]. [k] marks every pixel the layer covers: the coverage times
    // 100000, then the unit cube, is exactly 1 from a coverage of 1e-5 up
    // and exactly 0 where there is none. The last merge then keeps the
    // composite bit for bit wherever the layer is absent, rather than
    // sending it through the view and back, and takes [w] whole wherever it
    // is present.
    graph << ",split[c" << ordinal << "][m" << ordinal << "];[m" << ordinal
          << "]colorchannelmixer=rr=0:ra=1:gg=0:ga=1:bb=0:ba=1,split[a"
          << ordinal << "][q" << ordinal << "];[q" << ordinal
          << "]colorlevels=rimax=0.00001:gimax=0.00001:bimax=0.00001,"
             "lut3d=file='"
          << layer.coverage_cube_path << "':interp=trilinear[k" << ordinal
          << "];[base" << ordinal << "]split[b" << ordinal << "][v" << ordinal
          << "];[v" << ordinal << ']';
    if (!layer.view_shaper_path.empty()) {
        graph << "lut1d=file='" << layer.view_shaper_path << "':interp=linear,";
    }
    // The view is bounded to [0, 1], what it shows, as the monitor's
    // displaySignal is: the display cube returns up to 1.12 for a bright
    // input, and a highlight mixed unbounded came out 0.06 brighter than
    // Record under a half-strength dip (monitor_export_parity_tests). The
    // unit cube clamps exactly.
    graph << "lut3d=file='" << layer.view_cube_path
          << "':interp=trilinear,lut3d=file='" << layer.coverage_cube_path
          << "':interp=trilinear[d" << ordinal << "];[d" << ordinal << "][c"
          << ordinal << "][a" << ordinal << "]maskedmerge=planes=7,lut3d=file='"
          << layer.cube_path << "':interp=trilinear[w" << ordinal << "];[b"
          << ordinal << "][w" << ordinal << "][k" << ordinal
          << "]maskedmerge=planes=7:enable=" << window << "[base"
          << (ordinal + 1) << "];";
    appendVeils(ordinal + 1);
}

int64_t ExactFrames(const RationalTime& duration, MediaRate rate,
                    const char* what = "timeline duration") {
    const __int128 numerator = static_cast<__int128>(duration.value) * rate.num;
    const __int128 denominator =
        static_cast<__int128>(duration.rate) * rate.den;
    if (numerator % denominator != 0) {
        throw std::invalid_argument(std::string(what) +
                                    " must contain whole export frames");
    }
    const __int128 frames = numerator / denominator;
    if (frames > std::numeric_limits<int64_t>::max()) {
        throw std::overflow_error("export frame count overflows int64_t");
    }
    return static_cast<int64_t>(frames);
}

std::filesystem::path ResolveMediaPath(
    const std::filesystem::path& baseDirectory, const DocumentSource& source) {
    std::filesystem::path path(source.path);
    if (path.is_relative()) path = baseDirectory / path;
    return std::filesystem::absolute(path).lexically_normal();
}

std::string TemporaryOutputPath(const std::filesystem::path& destination) {
    const std::string filename = destination.stem().string() + ".cutmachine-" +
                                 GenerateUlid() + ".part" +
                                 destination.extension().string();
    return (destination.parent_path() / filename).string();
}

// MOTION-2026-09 M4 -- renders one motion clip's PNG sequence with the
// renderer the executable installed (MotionFrameRenderer.h). Each distinct
// composition frame is rendered once; an output frame that shows it again
// (another cadence than the composition's) is a hard link to it.
bool RenderMotionAsset(const ExportMotionAsset& asset,
                       const std::atomic_bool* cancel, std::string& error) {
    const MotionFrameRenderer render = CurrentMotionFrameRenderer();
    if (render == nullptr) {
        error = std::string(
                    "this process has no motion renderer (Core Text and "
                    "Core Graphics) to export motion clips with: ") +
                kMotionRendererHolder;
        return false;
    }
    std::error_code status;
    if (!std::filesystem::create_directory(asset.directory, status) || status) {
        error = "unable to create the motion frame directory '" +
                asset.directory + "'";
        return false;
    }
    MotionComposition composition;
    MotionError motionError;
    if (!ParseDocumentComposition(asset.composition, composition,
                                  motionError)) {
        error = "composition '" + asset.composition.id +
                "' cannot be rendered: " + motionError.code + ": " +
                motionError.message;
        return false;
    }
    MotionFrameRequest request;
    request.composition = &composition;
    request.props = asset.props;
    request.media_directory = asset.media_directory;
    request.reach = asset.reach;
    request.scale = asset.render_scale;
    // REM2 -- the part of the composition's sound the clip shows.
    if (asset.audio) {
        MotionMediaMeasurer measurer(asset.media_directory,
                                     asset.reach.media_paths);
        MotionPcm mix;
        if (!MixMotionAudio(composition, asset.props, asset.reach.Context(),
                            measurer, asset.audio_first, asset.audio_count, mix,
                            motionError)) {
            error =
                "composition '" + asset.composition.id +
                "' sound: " + motionError.code +
                (motionError.path.empty() ? "" : " at " + motionError.path) +
                ": " + motionError.message;
            return false;
        }
        if (!WriteMotionAudioWav(mix, asset.audio_path, error)) {
            return false;
        }
    }
    // MOTION-DESIGN-2026-09 -- each distinct composition frame rendered
    // once, on every core (MotionParallelRender.h), then the clip's numbered
    // sequence linked to those renders: a held or slowed frame repeats a
    // file, it is not drawn twice.
    std::vector<int64_t> distinct;
    {
        std::set<int64_t> seen;
        for (const int64_t frame : asset.frames) {
            if (seen.insert(frame).second) {
                distinct.push_back(frame);
            }
        }
    }
    const auto renderedPath = [&](int64_t frame) {
        char name[40];
        std::snprintf(name, sizeof(name), "/c%06lld.png",
                      static_cast<long long>(frame));
        return asset.directory + name;
    };
    std::string renderError;
    if (!RenderMotionFramesToFiles(request, distinct, renderedPath, cancel,
                                   renderError)) {
        error = "composition '" + asset.composition.id + "' " + renderError;
        return false;
    }
    for (size_t index = 0; index < asset.frames.size(); ++index) {
        char name[32];
        std::snprintf(name, sizeof(name), "/%06zu.png", index);
        const std::string path = asset.directory + name;
        const std::string source = renderedPath(asset.frames[index]);
        if (link(source.c_str(), path.c_str()) == 0) {
            continue;
        }
        std::filesystem::copy_file(source, path, status);
        if (status) {
            error = "unable to place motion frame " +
                    std::to_string(asset.frames[index]) + ": " +
                    status.message();
            return false;
        }
    }
    return true;
}

// LG3 -- docs/roadmap/LEGENDES-2026-10.md. Draws the window's cartons with
// the caption renderer: their texts broken for delivery, each distinct
// picture once, then one numbered file per frame, a hard link to the
// picture it shows, as RenderMotionAsset places a motion clip's frames.
bool RenderCaptionAsset(const ExportCaptionAsset& asset,
                        const std::atomic_bool* cancel, std::string& error) {
    const CaptionRenderer* renderer = CurrentCaptionRenderer();
    if (renderer == nullptr) {
        error = kNoCaptionRenderer;
        return false;
    }
    std::error_code status;
    if (!std::filesystem::create_directory(asset.directory, status) || status) {
        error = "unable to create the caption frame directory '" +
                asset.directory + "'";
        return false;
    }
    // Captions go out as delivered: the renderer never breaks a line itself.
    std::vector<CaptionStyle> styles;
    std::vector<std::string> texts;
    for (const CaptionCarton& carton : asset.cartons) {
        styles.push_back(carton.style);
        texts.push_back(carton.text);
    }
    DocumentSequence sequence;
    sequence.width = asset.sequence_width;
    if (!WrapCaptionTextsForDelivery(styles, sequence, texts, error)) {
        error.insert(0, "unable to fit the captions to the frame: ");
        return false;
    }
    std::vector<std::vector<CaptionCarton>> pictures;
    for (const std::vector<size_t>& shown : asset.pictures) {
        std::vector<CaptionCarton> cartons;
        const size_t picture = pictures.size();
        for (size_t place = 0; place < shown.size(); ++place) {
            const size_t index = shown[place];
            CaptionCarton carton = asset.cartons[index];
            carton.text = texts[index];
            // PA4 -- the state of its words in this picture.
            if (picture < asset.picture_words.size() &&
                place < asset.picture_words[picture].size())
                carton.words_spoken = asset.picture_words[picture][place];
            cartons.push_back(std::move(carton));
        }
        pictures.push_back(std::move(cartons));
    }
    const auto picturePath = [&](size_t picture) {
        char name[40];
        std::snprintf(name, sizeof(name), "/p%06zu.png", picture);
        return asset.directory + name;
    };
    CaptionDrawing drawing;
    drawing.width = asset.width;
    drawing.height = asset.height;
    drawing.sequence_width = asset.sequence_width;
    std::string failure;
    const bool drawn = renderer->draw(
        drawing, pictures,
        [&](size_t picture, CaptionImage& image) {
            if (cancel != nullptr && cancel->load()) {
                failure = "export cancelled";
                return false;
            }
            std::string png;
            if (!renderer->encode_png(image, png, failure)) return false;
            std::ofstream file(picturePath(picture),
                               std::ios::binary | std::ios::trunc);
            file.write(png.data(), static_cast<std::streamsize>(png.size()));
            file.close();
            if (!file) {
                failure = "unable to write caption picture " +
                          std::to_string(picture);
                return false;
            }
            return true;
        },
        error);
    if (!drawn || !failure.empty()) {
        error = "unable to draw the captions: " +
                (failure.empty() ? error : failure);
        return false;
    }
    for (size_t index = 0; index < asset.frames.size(); ++index) {
        char name[32];
        std::snprintf(name, sizeof(name), "/%06zu.png", index);
        const std::string path = asset.directory + name;
        const std::string source = picturePath(asset.frames[index]);
        if (link(source.c_str(), path.c_str()) == 0) {
            continue;
        }
        std::filesystem::copy_file(source, path, status);
        if (status) {
            error = "unable to place caption frame " + std::to_string(index) +
                    ": " + status.message();
            return false;
        }
    }
    return true;
}

bool ParseProgressLine(std::string_view line, ExportProgress& progress) {
    const size_t separator = line.find('=');
    if (separator == std::string_view::npos) return false;
    const std::string key(line.substr(0, separator));
    const std::string value(line.substr(separator + 1));
    try {
        if (key == "frame")
            progress.rendered_frames = std::stoll(value);
        else if (key == "out_time_us")
            progress.output_time_us = std::stoll(value);
        else if (key == "speed") {
            std::string number = value;
            if (!number.empty() && number.back() == 'x') number.pop_back();
            progress.speed = std::stod(number);
        }
    } catch (const std::exception&) {
        // Progress is advisory. A malformed line must not fail the render.
    }
    if (progress.total_frames > 0) {
        progress.fraction =
            std::clamp(static_cast<double>(progress.rendered_frames) /
                           progress.total_frames,
                       0.0, 1.0);
    }
    return key == "progress";
}

// PRD2 -- what the plan's command lines, and those of its segments, ask of
// the FFmpeg they will run (FfmpegCapabilities.h).
bool RequireCapabilities(const ExportPlan& plan, std::string& error) {
    if (!ffmpeg::Require(plan.settings.ffmpeg_path, plan.arguments, error)) {
        return false;
    }
    for (const ExportPlan& segment : plan.segments) {
        if (!RequireCapabilities(segment, error)) {
            return false;
        }
    }
    return true;
}

}  // namespace

const std::vector<ExportPresetDescriptor>& Exporter::Presets() {
    static const std::vector<ExportPresetDescriptor> presets = {
        {ExportPresetId::HevcHighQuality, "hevc_high_quality",
         "H.265 Main10 — Haute qualité",
         "Format de séquence, encodage matériel rapide"},
        {ExportPresetId::HevcMaster, "hevc_master",
         "H.265 Main10 — Master x265",
         "Format de séquence, qualité constante CRF 18"},
        {ExportPresetId::HevcWeb1080, "hevc_web_1080",
         "H.265 Main10 — Web 1080p",
         "1920×1080, cadence de séquence, 20 Mbit/s"},
        {ExportPresetId::HevcUhdDelivery, "hevc_uhd_delivery",
         "H.265 Main10 — Livraison UHD",
         "3840×2160, cadence de séquence, 80 Mbit/s"},
        {ExportPresetId::H264, "h264", "H.264 High — Diffusion",
         "Format de séquence, 8 bits, encodage matériel, 20 Mbit/s"},
    };
    return presets;
}

ExportSettings Exporter::SettingsForPreset(ExportPresetId preset,
                                           const std::string& outputPath,
                                           int32_t sourceWidth,
                                           int32_t sourceHeight,
                                           MediaRate sourceRate) {
    if (preset == ExportPresetId::H264) {
        // PRD16 -- what export_timeline has always built for `codec: h264`
        // at the sequence's format and cadence, H264Preset's bit rate. An
        // odd side is rounded up, as every preset rounds it (4:2:0 has no
        // odd size): this is the default an SDR timeline gets on every
        // surface, and `--export` rounded an odd sequence before it took
        // this default. A codec named alone keeps its exact size, and its
        // refusal of an odd one.
        ExportSettings settings = H264Preset(outputPath);
        settings.width = sourceWidth > 0 ? sourceWidth : 1920;
        settings.height = sourceHeight > 0 ? sourceHeight : 1080;
        settings.width += settings.width % 2;
        settings.height += settings.height % 2;
        settings.frame_rate = sourceRate.num > 0 && sourceRate.den > 0
                                  ? sourceRate
                                  : MediaRate{25, 1};
        return settings;
    }
    ExportSettings settings;
    settings.output_path = outputPath;
    settings.width = sourceWidth > 0 ? sourceWidth : 1920;
    settings.height = sourceHeight > 0 ? sourceHeight : 1080;
    // Main10 4:2:0 requires even output dimensions.
    settings.width += settings.width % 2;
    settings.height += settings.height % 2;
    if (sourceRate.num > 0 && sourceRate.den > 0)
        settings.frame_rate = sourceRate;
    settings.encoder = ExportEncoder::HevcVideoToolbox;
    settings.video_bitrate = 45000000;
    settings.main10 = true;

    switch (preset) {
        case ExportPresetId::HevcHighQuality: {
            const long double relativeLoad =
                static_cast<long double>(settings.width) * settings.height *
                settings.frame_rate.num /
                (static_cast<long double>(1920) * 1080 * 25 *
                 settings.frame_rate.den);
            settings.video_bitrate = static_cast<int64_t>(std::clamp(
                45000000.0L * relativeLoad, 12000000.0L, 120000000.0L));
            break;
        }
        case ExportPresetId::HevcMaster:
            settings.encoder = ExportEncoder::HevcSoftware;
            break;
        case ExportPresetId::HevcWeb1080:
            settings.width = 1920;
            settings.height = 1080;
            settings.video_bitrate = 20000000;
            break;
        case ExportPresetId::HevcUhdDelivery:
            settings.width = 3840;
            settings.height = 2160;
            settings.video_bitrate = 80000000;
            break;
        case ExportPresetId::H264:
            // Built above.
            break;
    }
    return settings;
}

ExportSettings Exporter::SettingsForPreset(ExportPresetId preset,
                                           const Document& timeline,
                                           const std::string& outputPath) {
    return SettingsForPreset(preset, outputPath, timeline.sequence.width,
                             timeline.sequence.height,
                             timeline.sequence.frame_rate);
}

ExportPresetId Exporter::DefaultPreset(const Document& timeline) {
    // The tags the export writes say the same: BT.2020 and ARIB STD-B67 only
    // when colour management is on and its output is HLG.
    const ColorManagementSettings& color = timeline.color_management;
    return color.enabled && color.output_transfer == "hlg"
               ? ExportPresetId::HevcHighQuality
               : ExportPresetId::H264;
}

ExportSettings Exporter::DefaultSettings(const Document& timeline,
                                         const std::string& outputPath) {
    return SettingsForPreset(DefaultPreset(timeline), timeline, outputPath);
}

size_t Exporter::PresetIndex(ExportPresetId preset) {
    const std::vector<ExportPresetDescriptor>& presets = Presets();
    for (size_t index = 0; index < presets.size(); ++index) {
        if (presets[index].id == preset) {
            return index;
        }
    }
    return 0;
}

bool Exporter::FindPreset(const std::string& key, ExportPresetId& preset) {
    for (const ExportPresetDescriptor& descriptor : Presets()) {
        if (key == descriptor.key) {
            preset = descriptor.id;
            return true;
        }
    }
    return false;
}

std::string Exporter::PresetKeys() {
    std::string keys;
    for (const ExportPresetDescriptor& descriptor : Presets()) {
        if (!keys.empty()) {
            keys += ", ";
        }
        keys += descriptor.key;
    }
    return keys;
}

bool Exporter::ReadPreset(const std::string& key, const std::string& codec,
                          const std::string& argument, ExportPresetId& preset,
                          std::string& error) {
    if (!FindPreset(key, preset)) {
        error = "'" + argument + "' must be one of " + PresetKeys() +
                ", not '" + key + "'";
        return false;
    }
    const bool h264 = preset == ExportPresetId::H264;
    if (!codec.empty() && (codec == "h264") != h264) {
        error = "'" + argument + "' " + key + " encodes " +
                (h264 ? "H.264" : "HEVC") + " and contradicts codec '" + codec +
                "': give the preset alone, or with the codec it encodes";
        return false;
    }
    return true;
}

void Exporter::UseSoftwareEncoder(ExportSettings& settings) {
    if (IsH264(settings.encoder)) {
        settings.encoder = ExportEncoder::H264Software;
    } else if (settings.encoder == ExportEncoder::HevcVideoToolbox) {
        settings.encoder = ExportEncoder::HevcSoftware;
    }
}

std::string Exporter::DescribeTimelines(const Project& project) {
    std::string list;
    for (const DocumentSequence& timeline : project.timelines) {
        if (!list.empty()) {
            list += ", ";
        }
        list += "'" + timeline.name + "' (" + timeline.id + ")";
    }
    return list;
}

ExportSettings Exporter::HevcMain10Preset(const std::string& outputPath) {
    ExportSettings settings = SettingsForPreset(
        ExportPresetId::HevcHighQuality, outputPath, 1920, 1080, {25, 1});
    // The headless preset has no document at construction time. Zero is a
    // deliberate deferred value resolved by ExportCommand from the active
    // sequence, matching the UI's "Format de séquence" preset.
    settings.width = 0;
    settings.height = 0;
    settings.frame_rate = {0, 1};
    return settings;
}

ExportSettings Exporter::HevcMain10SoftwarePreset(
    const std::string& outputPath) {
    ExportSettings settings = HevcMain10Preset(outputPath);
    settings.encoder = ExportEncoder::HevcSoftware;
    return settings;
}

namespace {

// PA2 -- a level in dB or LUFS to a hundredth, the meter's own resolution
// being a tenth: "-14", "-13.96", "3.5". Classic locale, as every number
// handed to FFmpeg or written to an answer is.
std::string LoudnessDecimal(double value) {
    std::ostringstream text;
    text.imbue(std::locale::classic());
    text << std::fixed << std::setprecision(2) << value;
    std::string spelled = text.str();
    while (spelled.back() == '0') spelled.pop_back();
    if (spelled.back() == '.') spelled.pop_back();
    if (spelled == "-0") spelled = "0";
    return spelled;
}

}  // namespace

std::string Exporter::ResultJson(const ExportPlan& plan) {
    using mcp_json::Value;
    Value result = Value::MakeObject();
    const bool h264 = IsH264(plan.settings.encoder);
    result.Set("ok", Value::MakeBool(true));
    result.Set("codec", Value::MakeString(h264 ? "h264" : "hevc"));
    result.Set("profile",
               Value::MakeString(
                   h264 ? "high" : (plan.settings.main10 ? "main10" : "main")));
    result.Set("timeline_id", Value::MakeString(plan.timeline_id));
    result.Set("range", Value::MakeString("entire"));
    result.Set("width", Value::MakeInt(plan.settings.width));
    result.Set("height", Value::MakeInt(plan.settings.height));
    Value rate = Value::MakeObject();
    rate.Set("num", Value::MakeInt(plan.settings.frame_rate.num));
    rate.Set("den", Value::MakeInt(plan.settings.frame_rate.den));
    result.Set("frame_rate", std::move(rate));
    result.Set("frames", Value::MakeInt(plan.total_frames));
    result.Set("path", Value::MakeString(plan.settings.output_path));
    if (!plan.caption_font_substitutions.empty()) {
        result.Set(
            "caption_fonts_substituted",
            CaptionFontSubstitutionsJson(plan.caption_font_substitutions));
    }
    // PA2 -- what the loudness target measured and did, once Run has.
    if (plan.loudness_outcome && plan.loudness_outcome->applied) {
        const ExportLoudnessOutcome& outcome = *plan.loudness_outcome;
        const auto number = [](const std::optional<double>& value) {
            return value ? Value::MakeRawNumber(LoudnessDecimal(*value))
                         : Value::MakeNull();
        };
        Value loudness = Value::MakeObject();
        loudness.Set("target_lufs", Value::MakeRawNumber(
                                        LoudnessDecimal(outcome.target_lufs)));
        loudness.Set("silent", Value::MakeBool(outcome.silent));
        loudness.Set("mix_lufs", number(outcome.mix_lufs));
        loudness.Set("mix_true_peak_dbfs", number(outcome.mix_true_peak_dbfs));
        loudness.Set("gain_db",
                     Value::MakeRawNumber(LoudnessDecimal(outcome.gain_db)));
        loudness.Set("delivered_lufs", number(outcome.delivered_lufs));
        loudness.Set("delivered_true_peak_dbfs",
                     number(outcome.delivered_true_peak_dbfs));
        result.Set("loudness", std::move(loudness));
    }
    return result.Dump();
}

bool Exporter::ExportBatch(
    const Project& project, const std::string& projectPath,
    const std::vector<std::pair<Ulid, ExportSettings>>& items, bool preview,
    std::string& resultJson, std::string& errorName, std::string& error,
    const MediaTaskRunner& runner) {
    using mcp_json::Value;
    resultJson.clear();
    error.clear();
    errorName = "InvalidExport";
    if (items.empty()) {
        error = "export batch is empty";
        return false;
    }
    std::set<std::filesystem::path> destinations;
    std::vector<ExportPlan> plans;
    for (const auto& [id, requested] : items) {
        const auto* timeline = project.FindTimeline(id);
        if (timeline == nullptr) {
            errorName = "UnknownSequence";
            // PRD16 -- the timelines there are, so the caller corrects in
            // one turn.
            error = "unknown export timeline_id '" + id +
                    "'; the project's timelines: " + DescribeTimelines(project);
            return false;
        }
        std::error_code pathError;
        const auto path = std::filesystem::weakly_canonical(
            std::filesystem::absolute(requested.output_path), pathError);
        if (pathError || !destinations.insert(path).second) {
            error = "batch destinations must be distinct resolved paths";
            return false;
        }
        ExportSettings settings = requested;
        // PRD16 -- zero dimensions, as a codec named alone leaves them, are
        // the timeline's exact format; a preset that sets its own size
        // (hevc_web_1080, hevc_uhd_delivery) keeps it.
        if (settings.width == 0 && settings.height == 0) {
            settings.width = timeline->width;
            settings.height = timeline->height;
        }
        if (settings.frame_rate.num == 0) {
            settings.frame_rate = timeline->frame_rate;
        }
        ExportPlan plan;
        if (!BuildPlan(project.MakeDocument(id), projectPath, settings, plan,
                       error)) {
            error.insert(0, ": ");
            error.insert(0, id);
            error.insert(0, "timeline ");
            return false;
        }
        plans.push_back(std::move(plan));
    }
    // PRD2 -- validated with the rest: a batch whose third file needs an
    // encoder this FFmpeg lacks is refused before its first is rendered,
    // and a preview says so.
    for (const ExportPlan& plan : plans) {
        if (!RequireCapabilities(plan, error)) {
            return false;
        }
    }
    Value report = Value::MakeObject();
    Value outputs = Value::MakeArray();
    report.Set("preview", Value::MakeBool(preview));
    report.Set("total", Value::MakeInt(plans.size()));
    int64_t completed = 0;
    for (const auto& plan : plans) {
        Value item;
        std::string parseError;
        Value::Parse(ResultJson(plan), item, parseError);
        const ExportTaskStep step{static_cast<size_t>(completed), plans.size()};
        if (!preview && !RunPlan(plan, runner, step, error)) {
            item.Set("ok", Value::MakeBool(false));
            item.Set("detail", Value::MakeString(error));
            outputs.Push(std::move(item));
            report.Set("ok", Value::MakeBool(false));
            report.Set("completed", Value::MakeInt(completed));
            report.Set("outputs", std::move(outputs));
            resultJson = report.Dump();
            errorName = "ExportFailed";
            return false;
        }
        item.Set("status", Value::MakeString(preview ? "planned" : "complete"));
        outputs.Push(std::move(item));
        if (!preview) {
            ++completed;
        }
    }
    report.Set("ok", Value::MakeBool(true));
    report.Set("completed", Value::MakeInt(completed));
    report.Set("outputs", std::move(outputs));
    resultJson = report.Dump();
    errorName.clear();
    return true;
}

namespace {

// The codec and container arguments of a render. Shared by a single pass
// and by the pass that assembles segments, so a film rendered by segments is
// encoded exactly as one rendered at once.
bool AppendEncodingArguments(const Document& document,
                             const ExportSettings& settings,
                             std::vector<std::string>& arguments,
                             std::string& error) {
    const bool inspection =
        settings.encoder == ExportEncoder::LosslessInspection;
    if (settings.encoder == ExportEncoder::HevcVideoToolbox) {
        arguments.insert(
            arguments.end(),
            {"-c:v", "hevc_videotoolbox", "-profile:v",
             settings.main10 ? "main10" : "main", "-b:v",
             std::to_string(settings.video_bitrate), "-allow_sw", "1"});
    } else if (settings.encoder == ExportEncoder::HevcSoftware) {
        arguments.insert(arguments.end(), {"-c:v", "libx265", "-profile:v",
                                           settings.main10 ? "main10" : "main",
                                           "-preset", "medium", "-crf", "18"});
    } else if (settings.encoder == ExportEncoder::H264VideoToolbox) {
        arguments.insert(
            arguments.end(),
            {"-c:v", "h264_videotoolbox", "-profile:v", "high", "-b:v",
             std::to_string(settings.video_bitrate), "-allow_sw", "1"});
    } else if (settings.encoder == ExportEncoder::H264Software) {
        arguments.insert(arguments.end(),
                         {"-c:v", "libx264", "-profile:v", "high", "-preset",
                          "medium", "-crf", "18"});
    } else if (inspection) {
        arguments.insert(arguments.end(), {"-c:v", "ffv1", "-level", "3"});
    } else {
        error = "unsupported export encoder";
        return false;
    }
    if (inspection) {
        // IA4 -- the samples as the mix made them, and no container flag a
        // delivery needs.
        arguments.insert(
            arguments.end(),
            {"-pix_fmt", settings.main10 ? "yuv420p10le" : "yuv420p", "-c:a",
             "pcm_f32le", "-ar", std::to_string(settings.audio_sample_rate)});
    } else {
        arguments.insert(
            arguments.end(),
            {"-pix_fmt", settings.main10 ? "yuv420p10le" : "yuv420p", "-tag:v",
             Exporter::IsH264(settings.encoder) ? "avc1" : "hvc1",
             "-color_primaries",
             document.color_management.enabled &&
                     document.color_management.output_gamut == "rec2020"
                 ? "bt2020"
                 : "bt709",
             "-color_trc",
             document.color_management.enabled &&
                     document.color_management.output_transfer == "hlg"
                 ? "arib-std-b67"
                 : (settings.sdr_tag == ExportSdrTag::Rec709A ? "unknown"
                                                              : "bt709"),
             "-colorspace",
             document.color_management.enabled &&
                     document.color_management.output_gamut == "rec2020"
                 ? "bt2020nc"
                 : "bt709",
             "-color_range", "tv"});
        if (settings.loudness.requested) {
            // PA2 -- the sound of a delivery with a loudness target stays
            // lossless until its gain is known (ApplyLoudnessTarget), so
            // that it is encoded once.
            arguments.insert(arguments.end(),
                             {"-c:a", "alac", "-sample_fmt", "s32p"});
        } else {
            arguments.insert(arguments.end(),
                             {"-c:a", "aac", "-b:a",
                              std::to_string(settings.audio_bitrate)});
        }
        arguments.insert(
            arguments.end(),
            {"-ar", std::to_string(settings.audio_sample_rate), "-movflags",
             settings.sdr_tag == ExportSdrTag::Rec709A
                 ? "+faststart+write_colr+write_gama"
                 : "+faststart"});
        // Rec.709-A: the gama atom Resolve writes beside colr 1-2-1.
        if (settings.sdr_tag == ExportSdrTag::Rec709A)
            arguments.insert(arguments.end(), {"-mov_gamma", "2.4"});
    }
    return true;
}

// See ExportSettings::max_inputs_per_pass. Eight 4K XAVC-I decoders stay
// under 6 GB with FFmpeg's own thread count.
constexpr size_t kMaximumInputsPerPass = 8;

// AX-2026-09 -- that limit is a memory budget, measured on 3840x2160
// pictures, and was applied to every timeline as a count. A 57 s interview
// in 1080x1920 (48 A/V pairs, 17 cutaways) was cut into one-second windows,
// one FFmpeg each: 12 minutes of export, a third of an agent's run. The
// budget stays; the count follows the largest picture the timeline reads,
// and falls back to the measured eight when a size is unknown.
constexpr int64_t kDecoderPixelBudget =
    static_cast<int64_t>(kMaximumInputsPerPass) * 3840 * 2160;
constexpr size_t kInputsPerPassCeiling = 32;

size_t DefaultInputsPerPass(const Document& document) {
    // A tuning knob for measuring the budget on a machine, never needed to
    // export correctly.
    if (const char* forced = std::getenv("CUTMACHINE_EXPORT_INPUTS_PER_PASS")) {
        const long value = std::strtol(forced, nullptr, 10);
        if (value > 0) return static_cast<size_t>(value);
    }
    int64_t largest = 0;
    for (const DocumentTrack& track : document.sequence.tracks) {
        for (const DocumentClip& clip : track.clips) {
            const LibraryMedia* media =
                document.FindLibraryMedia(clip.source_id);
            if (media == nullptr || !media->has_video) continue;
            const int64_t pixels = static_cast<int64_t>(media->width) *
                                   static_cast<int64_t>(media->height);
            if (pixels <= 0) return kMaximumInputsPerPass;
            largest = std::max(largest, pixels);
        }
    }
    if (largest <= 0) return kMaximumInputsPerPass;
    return std::clamp<size_t>(
        static_cast<size_t>(kDecoderPixelBudget / largest),
        kMaximumInputsPerPass, kInputsPerPassCeiling);
}
// A segment starts at ten seconds of 25 fps and is halved until it opens no
// more inputs than the limit.
constexpr int64_t kSegmentFrames = 250;

size_t CountInputs(const std::vector<std::string>& arguments) {
    return static_cast<size_t>(
        std::count(arguments.begin(), arguments.end(), std::string("-i")));
}

// Turns a single-pass plan of the whole timeline into segments and an
// assembly pass. Each segment is the IA4 window of the same graph, rendered
// without loss in the delivery's pixel format; the sound of consecutive
// windows is cut at the same samples, so the segments laid end to end are
// the film. The segments sit beside the destination, hidden, like the
// temporary output, and are removed by Run.
bool BuildSegments(const Document& document, const std::string& documentPath,
                   const ExportSettings& settings, size_t limit,
                   ExportPlan& plan, std::string& error) {
    const std::filesystem::path destination(plan.settings.output_path);
    const std::string prefix = (destination.parent_path() /
                                ("." + destination.stem().string() +
                                 ".cutmachine-" + GenerateUlid() + "-segment-"))
                                   .string();
    plan.segment_directory = destination.parent_path().string();
    plan.segment_list_path = prefix + "list.txt";
    int64_t first = 0;
    while (first < plan.total_frames) {
        int64_t length =
            std::min<int64_t>(kSegmentFrames, plan.total_frames - first);
        ExportPlan segment;
        for (;;) {
            ExportSettings window = settings;
            window.window_first = first;
            window.window_end = first + length;
            window.encoder = ExportEncoder::LosslessInspection;
            window.overwrite = true;
            window.max_inputs_per_pass = 0;
            char name[32];
            std::snprintf(name, sizeof(name), "%05zu.mkv",
                          plan.segments.size());
            window.output_path = prefix + name;
            if (!Exporter::BuildPlan(document, documentPath, window, segment,
                                     error)) {
                error =
                    "segment at frame " + std::to_string(first) + ": " + error;
                return false;
            }
            if (CountInputs(segment.arguments) <= limit || length == 1) {
                break;
            }
            length = (length + 1) / 2;
        }
        plan.segments.push_back(std::move(segment));
        first += length;
    }
    const std::string rate = std::to_string(settings.frame_rate.num) + "/" +
                             std::to_string(settings.frame_rate.den);
    std::vector<std::string> arguments = {
        "-hide_banner", "-nostdin", "-y",   "-progress",
        "pipe:1",       "-nostats", "-f",   "concat",
        "-safe",        "0",        "-i",   plan.segment_list_path,
        "-map",         "0:v:0",    "-map", "0:a:0"};
    if (!AppendEncodingArguments(document, settings, arguments, error)) {
        return false;
    }
    arguments.insert(arguments.end(), {"-r", rate, "-frames:v",
                                       std::to_string(plan.total_frames),
                                       plan.temporary_output_path});
    plan.arguments = std::move(arguments);
    // The single pass's derived files belong to the segments now.
    plan.temporary_lut_path.clear();
    plan.color_lut.clear();
    plan.caption_assets.clear();
    plan.clip_grade_luts.clear();
    plan.display_shaper = {};
    plan.motion_assets.clear();
    plan.matte_assets.clear();
    return true;
}

// Renders the segments, then the pass that assembles them. Every segment
// and the list are removed whatever happens; progress counts the segments
// and the assembly as one run.
bool RunSegments(const ExportPlan& plan, const ExportProgressCallback& callback,
                 const std::atomic_bool* cancel, std::string& error) {
    std::vector<std::string> produced;
    const auto cleanup = [&]() {
        std::error_code ignored;
        for (const std::string& path : produced) {
            std::filesystem::remove(path, ignored);
        }
        std::filesystem::remove(plan.segment_list_path, ignored);
    };
    const int64_t total = plan.total_frames * 2;
    int64_t done = 0;
    // PRD40 -- the pass the frames belong to, and its frames counted against
    // the film's: get_task read "frames 2000/2978" of a 1489-frame film.
    std::string pass;
    int64_t passDone = 0;
    const auto forward = [&](const ExportProgress& progress) {
        if (!callback) {
            return;
        }
        ExportProgress overall = progress;
        overall.rendered_frames = done + progress.rendered_frames;
        overall.total_frames = total;
        overall.fraction =
            total > 0 ? static_cast<double>(overall.rendered_frames) / total
                      : 0.0;
        overall.pass = pass;
        overall.pass_frames = passDone + progress.rendered_frames;
        overall.pass_total = plan.total_frames;
        callback(overall);
    };
    for (size_t index = 0; index < plan.segments.size(); ++index) {
        const ExportPlan& segment = plan.segments[index];
        pass = "segment " + std::to_string(index + 1) + "/" +
               std::to_string(plan.segments.size());
        passDone = done;
        produced.push_back(segment.settings.output_path);
        if (!Exporter::Run(segment, forward, cancel, error)) {
            cleanup();
            error = "segment " + std::to_string(index + 1) + " of " +
                    std::to_string(plan.segments.size()) + " (frames " +
                    std::to_string(segment.settings.window_first) + " to " +
                    std::to_string(segment.settings.window_end) + "): " + error;
            return false;
        }
        done += segment.total_frames;
    }
    {
        std::ofstream list(plan.segment_list_path, std::ios::trunc);
        for (const std::string& path : produced) {
            std::string quoted;
            for (const char c : path) {
                quoted += c == '\'' ? std::string("'\\''") : std::string(1, c);
            }
            list << "file '" << quoted << "'\n";
        }
        list.close();
        if (!list) {
            cleanup();
            error = "unable to write the list of export segments";
            return false;
        }
    }
    ExportPlan assembly = plan;
    assembly.segments.clear();
    pass = "assembly";
    passDone = 0;
    const bool assembled = Exporter::Run(assembly, forward, cancel, error);
    cleanup();
    if (!assembled) {
        error = "assembling " + std::to_string(plan.segments.size()) +
                " segments: " + error;
    }
    return assembled;
}

}  // namespace

bool Exporter::BuildPlan(const Document& document,
                         const std::string& documentPath,
                         const ExportSettings& settings, ExportPlan& output,
                         std::string& error) {
    error.clear();
    output = {};
    if (IsH264(settings.encoder) && settings.main10) {
        error = "H.264 delivery requires an 8-bit profile";
        return false;
    }
    if (settings.output_path.empty()) {
        error = "export destination is empty";
        return false;
    }
    if (settings.loudness.requested && (settings.loudness.integrated_millilufs <
                                            kMinimumLoudnessTargetMillilufs ||
                                        settings.loudness.integrated_millilufs >
                                            kMaximumLoudnessTargetMillilufs)) {
        error = "the loudness target must lie between -40 and -5 LUFS";
        return false;
    }
    if (settings.width < 2 || settings.height < 2 || settings.width > 16384 ||
        settings.height > 16384 || settings.width % 2 || settings.height % 2) {
        error = "export dimensions must be even and between 2 and 16384";
        return false;
    }
    if (settings.frame_rate.num <= 0 || settings.frame_rate.den <= 0 ||
        settings.frame_rate.num > 240000 || settings.frame_rate.den > 10000) {
        error = "export frame rate is invalid";
        return false;
    }
    if (settings.video_bitrate <= 0 || settings.audio_bitrate <= 0 ||
        settings.audio_sample_rate <= 0) {
        error = "export bitrate and audio sample rate must be positive";
        return false;
    }
    const std::filesystem::path destination =
        std::filesystem::absolute(settings.output_path).lexically_normal();
    const std::string extension = destination.extension().string();
    // IA4 -- an inspection render is Matroska, and only it is.
    const bool inspection =
        settings.encoder == ExportEncoder::LosslessInspection;
    if (inspection && extension != ".mkv") {
        error = "a lossless inspection render is written as .mkv";
        return false;
    }
    if (!inspection && extension != ".mp4" && extension != ".MP4" &&
        extension != ".mov" && extension != ".MOV") {
        error = "export destination must use .mp4 or .mov";
        return false;
    }
    const bool rec709a = settings.sdr_tag == ExportSdrTag::Rec709A;
    if (rec709a && extension != ".mov" && extension != ".MOV") {
        error =
            "Rec.709-A needs a QuickTime .mov: its gama atom is what tells "
            "macOS to show the picture at gamma 2.4, and an .mp4 has none";
        return false;
    }
    if (rec709a && document.color_management.enabled &&
        document.color_management.output_transfer == "hlg") {
        error = "Rec.709-A names an SDR delivery; this timeline delivers HLG";
        return false;
    }
    std::error_code filesystemError;
    if (!std::filesystem::exists(destination.parent_path(), filesystemError) ||
        filesystemError) {
        error = "export destination directory does not exist";
        return false;
    }
    if (!settings.overwrite &&
        std::filesystem::exists(destination, filesystemError)) {
        error = "export destination already exists";
        return false;
    }
    if (filesystemError) {
        error = "unable to inspect export destination: " +
                filesystemError.message();
        return false;
    }

    RationalTime duration;
    try {
        duration = Timeline(document).Duration();
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
    if (duration.value <= 0) {
        error = "cannot export an empty timeline";
        return false;
    }
    // IA4 -- docs/roadmap/IA-2026-09.md. The window, checked against the
    // timeline and turned into the two exact times every consumer below
    // reads. Without one, [0, duration): the graph an export has always had.
    int64_t windowFirst = 0;
    int64_t windowEnd = 0;
    try {
        windowEnd = ExactFrames(duration, settings.frame_rate);
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
    const bool windowed =
        settings.window_end != 0 || settings.window_first != 0;
    if (windowed) {
        if (settings.window_first < 0 ||
            settings.window_end <= settings.window_first ||
            settings.window_end > windowEnd) {
            error = "export window [" + std::to_string(settings.window_first) +
                    ", " + std::to_string(settings.window_end) +
                    ") is not inside the timeline's " +
                    std::to_string(windowEnd) + " frames";
            return false;
        }
        windowFirst = settings.window_first;
        windowEnd = settings.window_end;
    }
    if (!settings.inspection_frames.empty()) {
        if (!inspection) {
            error = "only an inspection render keeps chosen frames";
            return false;
        }
        for (size_t index = 0; index < settings.inspection_frames.size();
             ++index) {
            const int64_t frame = settings.inspection_frames[index];
            if (frame < windowFirst || frame >= windowEnd ||
                (index > 0 && frame <= settings.inspection_frames[index - 1])) {
                error =
                    "inspection frames must be ascending and inside the "
                    "window";
                return false;
            }
        }
    }
    const RationalTime windowStart{windowFirst * settings.frame_rate.den,
                                   settings.frame_rate.num};
    const RationalTime windowStop{windowEnd * settings.frame_rate.den,
                                  settings.frame_rate.num};
    // A clip that ends before this, or starts at or after the window's end,
    // touches no frame and no sample of it. The sound keeps a second of
    // margin before the window: the limiter after the mix has a memory
    // (alimiter's release), and a clip that stops just before the window
    // can still set the gain of its first samples.
    const RationalTime audioLead =
        windowFirst > 0 ? windowStart.sub(RationalTime{1, 1}) : windowStart;

    const std::filesystem::path baseDirectory =
        std::filesystem::absolute(documentPath).parent_path();
    // R6 -- docs/roadmap/REEL-2026-09.md. The sequence's look, read from
    // the project package beside the document and checked against its
    // digest. FFmpeg is handed this very file: the monitor uploads the same
    // bytes, so the two engines share a definition rather than a copy of
    // one (guard rail 1). A missing file or a digest that no longer matches
    // is a refusal, not an export without the look.
    LoadedLookLut look;
    if (!LoadSequenceLookLut(document.sequence, document.color_management,
                             baseDirectory.string(), look, error)) {
        error = "unable to read the sequence's look LUT: " + error;
        return false;
    }
    // FI2 -- ROADMAP.md. Even a source outside the rendered window remains
    // project media: overwriting it would damage a later export.
    for (const DocumentSource& source : document.sources) {
        const std::filesystem::path path =
            ResolveMediaPath(baseDirectory, source);
        if (path == destination) {
            error = "export destination cannot replace source media";
            return false;
        }
    }

    std::map<Ulid, bool> sourceHasAudio;
    std::map<Ulid, bool> sourceHasVideo;
    std::map<Ulid, bool> sourceIsRgb;
    // COLOR-T -- the range and matrix tags each source's picture carries,
    // read as the monitor reads them (ResolveUntaggedColorInput).
    std::map<Ulid, std::pair<std::string, std::string>> sourceTags;
    // FI2 -- prepare a source only after its clip survives output-state,
    // multicam and window selection (including dissolve handles and the
    // audio limiter's lead). Offline media that contributes nothing must
    // neither stop the export nor be probed.
    const auto prepareSource = [&](const DocumentSource& source) {
        if (sourceHasVideo.count(source.id) != 0) {
            return true;
        }
        if (!ffmpeg::InputPresent(
                ResolveMediaPath(baseDirectory, source).string(), error)) {
            return false;
        }
        const LibraryMedia* media = document.FindLibraryMedia(source.id);
        if (media != nullptr && media->metadata_complete) {
            sourceHasAudio[source.id] = media->has_audio;
            sourceHasVideo[source.id] = media->has_video;
            sourceIsRgb[source.id] = IsRgbPixelFormat(media->pixel_format);
            sourceTags[source.id] = {media->color_range, media->color_space};
            return true;
        }
        LibraryMedia detected;
        std::string probeError;
        if (!ProbeMediaMetadata(
                ResolveMediaPath(baseDirectory, source).string(), detected,
                probeError)) {
            error = "unable to probe source media for export: " + probeError;
            return false;
        }
        sourceHasAudio[source.id] = detected.has_audio;
        sourceHasVideo[source.id] = detected.has_video;
        sourceIsRgb[source.id] = IsRgbPixelFormat(detected.pixel_format);
        sourceTags[source.id] = {detected.color_range, detected.color_space};
        return true;
    };

    struct InputClip {
        const DocumentTrack* track = nullptr;
        const DocumentClip* clip = nullptr;
        const DocumentSource* source = nullptr;
        RationalTime source_in;
        RationalTime duration;
        // R3 -- what the clip reads of its source: `duration` at unit
        // speed, `duration * speed` otherwise (ClipSpeed.h).
        RationalTime source_duration;
        RationalTime timeline_in;
        std::optional<RationalTime> fade_in;
        // R5 -- docs/roadmap/REEL-2026-09.md. The colour dips that cover
        // part of this clip, in absolute sequence frames. Empty on every
        // clip no dip touches, which then keeps the chain it had.
        std::vector<ExportColorFade> color_fades;
        // EF2 -- docs/roadmap/EFFETS-2026-10.md. A sound clip's edges, its
        // cross fades included, as AudioMixFilters.h resolves them for
        // every mix; the chain above already spans its handles.
        audio_mix::ClipAudioEdges audio_edges;
        int input_index = -1;
        bool video = false;
        bool audio = false;
        bool motion = false;
        // R1 -- docs/roadmap/REEL-2026-09.md. Where this clip's picture
        // lands in the frame and which part of its source fills it, as
        // ClipFraming.h resolved it -- never as this file works it out.
        // False for a clip that frames nothing, which then keeps the single
        // fitted-and-centred chain this graph has always had, and its bytes
        // with it.
        bool framed = false;
        ClipFramingGeometry framing;
        // R2 -- docs/roadmap/REEL-2026-09.md. The same rectangle, once per
        // output frame of the clip, as ClipAnimation.h sampled it. Empty
        // for a clip that animates nothing, which then keeps the single
        // constant chain above, and its bytes with it.
        bool animated = false;
        bool animated_opacity = false;
        std::vector<ClipFramingGeometry> framing_by_frame;
        std::vector<EffectParamValue> opacity_by_frame;
        // R3, criterion 4 -- docs/roadmap/REEL-2026-09.md. The source frame
        // each output frame of a ramped clip shows, as ClipSpeed.h's rule
        // names it, once per frame. VF1 -- docs/roadmap/VFR-2026-11.md: a
        // clip at unit speed whose source is counted at another rate than
        // the sequence is handed the same list, the rule being no cadence
        // `fps` resamples exactly once the source and the sequence differ.
        // Empty for every other clip.
        std::vector<int64_t> listed_frames;
        // VF1 -- the grid frames the chain reads, first and last
        // (SourceGrid.h), and how FFmpeg reads them.
        int64_t grid_first = 0;
        int64_t grid_last = 0;
        SourceGridRead grid_read;
    };
    // VF1 -- docs/roadmap/VFR-2026-11.md. The first picture of each source,
    // decoded once per plan: the origin of its grid (SourceGrid.h).
    std::map<Ulid, int64_t> firstPictures;
    const auto planGridRead = [&](InputClip& input, const DocumentClip& clip,
                                  const DocumentSource& source,
                                  std::string& planError) {
        const MediaRate sourceRate = source.rate;
        const MediaRate sequenceRate = settings.frame_rate;
        // The rule asked of the span the chain covers: a dissolve's handle
        // moves where the chain starts reading, not which rule applies.
        DocumentClip read = clip;
        read.source_in = input.source_in;
        const bool sameRate =
            static_cast<int64_t>(sourceRate.num) * sequenceRate.den ==
            static_cast<int64_t>(sequenceRate.num) * sourceRate.den;
        MediaRate gridRate = sourceRate;
        try {
            const int64_t frames = ExactFrames(input.duration, sequenceRate,
                                               "video clip duration");
            if (!input.listed_frames.empty()) {
                input.grid_first = input.listed_frames.front();
                input.grid_last = input.listed_frames.back();
            } else if (ClipIsFreeze(clip)) {
                input.grid_first =
                    ClipSourceFrameAt(read, RationalTime{0, 1}, sourceRate);
                input.grid_last = input.grid_first;
            } else if (ClipHasSpeed(clip)) {
                input.grid_first =
                    ClipSourceFrameAt(read, RationalTime{0, 1}, sourceRate);
                input.grid_last = ClipSourceFrameAt(
                    read,
                    RationalTime{(frames - 1) * sequenceRate.den,
                                 sequenceRate.num},
                    sourceRate);
            } else if (sameRate) {
                // One grid step is one sequence frame: the chain shows grid
                // frames first, first + 1... at the sequence's own rate.
                gridRate = sequenceRate;
                input.grid_first =
                    ClipSourceFrameAt(read, RationalTime{0, 1}, sourceRate);
                input.grid_last = input.grid_first + frames - 1;
            } else {
                input.listed_frames =
                    ClipSourceFrames(read, 0, frames, sequenceRate, sourceRate);
                input.grid_first = input.listed_frames.front();
                input.grid_last = input.listed_frames.back();
            }
        } catch (const std::exception& exception) {
            planError = "clip '" + clip.id + "': " + exception.what();
            return false;
        }
        auto known = firstPictures.find(source.id);
        if (known == firstPictures.end()) {
            int64_t pts = 0;
            std::string ignored;
            // A file FFmpeg cannot decode a picture from fails the export
            // when FFmpeg reads it; until then its grid is anchored at
            // zero, so that the plan can still be built and inspected.
            if (!FirstPicturePts(
                    ResolveMediaPath(baseDirectory, source).string(), pts,
                    ignored)) {
                pts = 0;
            }
            known = firstPictures.emplace(source.id, pts).first;
        }
        input.grid_read = SourceGridReadFor(
            known->second, gridRate, input.grid_first, input.grid_last, true);
        return true;
    };
    std::vector<const DocumentTrack*> tracks;
    for (const DocumentTrack& track : document.sequence.tracks)
        tracks.push_back(&track);
    std::stable_sort(tracks.begin(), tracks.end(),
                     [](const DocumentTrack* left, const DocumentTrack* right) {
                         return left->index < right->index;
                     });
    const bool hasAudioSolo = std::any_of(
        tracks.begin(), tracks.end(), [](const DocumentTrack* track) {
            return track->kind == "audio" && track->solo;
        });

    // F1.5 -- ROADMAP.md. Inactive multicam angles never become inputs: the
    // monitor and verify_export leave out the same clips (Multicam.h). Nor
    // do disabled clips, picture or sound (ClipEnabled.h): a dissolve on one
    // keeps its other side's extension and fade, as for an angle.
    const std::set<Ulid> offScreen = OffScreenClipIds(document.sequence);
    std::vector<InputClip> inputs;
    for (const DocumentTrack* track : tracks) {
        if (track->kind == "caption") continue;
        if ((track->kind == "video" && !track->visible) ||
            (track->kind == "audio" &&
             (track->muted || (hasAudioSolo && !track->solo))))
            continue;
        for (const DocumentClip& clip : track->clips) {
            // MOTION-2026-09 M4 -- a motion clip is a picture without a
            // media: rendered below, placed like any video clip.
            const bool motion = IsMotionClip(clip);
            const DocumentSource* source =
                motion ? nullptr : document.FindSource(clip.source_id);
            if (source == nullptr && !motion) {
                error = "clip references an unknown source";
                return false;
            }
            const bool video = track->kind == "video";
            const bool audio = !motion && track->kind == "audio";
            if (!video && !audio) continue;
            if (video && offScreen.count(clip.id) != 0) {
                continue;
            }
            if (audio && !ClipIsHeard(clip)) {
                continue;
            }
            InputClip input;
            input.track = track;
            input.clip = &clip;
            input.source = source;
            input.source_in = clip.source_in;
            input.duration = clip.duration;
            input.timeline_in = clip.timeline_in;
            input.input_index = static_cast<int>(inputs.size());
            input.video = video;
            input.audio = audio;
            input.motion = motion;
            if (video) {
                for (const DocumentTransition& transition :
                     document.sequence.transitions) {
                    if (transition.track_id != track->id) continue;
                    // R5 -- where the transition falls, by the one function
                    // Document::Validate, the monitor and verify_export
                    // also ask (guard rail 1). Validate has already
                    // refused a document this fails on.
                    TransitionSpan span;
                    std::string spanError;
                    if (!ResolveTransitionSpan(document.sequence, transition,
                                               span, spanError)) {
                        error =
                            "transition '" + transition.id + "' " + spanError;
                        return false;
                    }
                    const RationalTime pre{
                        span.pre_frames * document.sequence.frame_rate.den,
                        document.sequence.frame_rate.num};
                    const RationalTime post{
                        span.post_frames * document.sequence.frame_rate.den,
                        document.sequence.frame_rate.num};
                    // R5 -- a colour dip reads no handle and extends no
                    // clip: it is a ramp towards a colour on the shot that
                    // is already there, which is why the same object works
                    // at the edge of a track where a dissolve cannot.
                    if (TransitionIsColorDip(transition)) {
                        TransitionColor color;
                        if (!ParseTransitionColor(transition.color, color,
                                                  error)) {
                            error =
                                "transition '" + transition.id + "' " + error;
                            return false;
                        }
                        // R5 -- under colour management the veil is laid
                        // in the SDR view, the stage a graphic colour
                        // belongs to (AppendManagedLayer). Laid in the
                        // camera's encoding instead, a white dip on an
                        // S-Log3 rush was measured 0.26 away from the
                        // monitor (tests/monitor_export_parity_tests.mm).
                        //
                        // A dip veils the track; a translucent or fading
                        // clip is composited with what lies under it, and
                        // the veil would go under that composite instead
                        // of over it. Refused by name rather than
                        // delivered a shade off (guard rail 5).
                        if ((transition.left_clip_id == clip.id ||
                             transition.right_clip_id == clip.id) &&
                            (clip.opacity.num != clip.opacity.den ||
                             ClipAnimates(clip,
                                          ClipAnimatedParameter::Opacity))) {
                            error = "transition '" + transition.id +
                                    "' dips to a colour over clip '" + clip.id +
                                    "', whose opacity is not 1/1; the veil "
                                    "would composite under the clip instead "
                                    "of over the track (R5)";
                            return false;
                        }
                        if (transition.left_clip_id == clip.id &&
                            span.pre_frames > 0) {
                            ExportColorFade fade{span.first_frame(),
                                                 span.pre_frames, true, color};
                            for (int64_t index = 0; index < span.pre_frames;
                                 ++index) {
                                fade.alphas.push_back(
                                    ColorVeilAlpha(span, index));
                            }
                            input.color_fades.push_back(std::move(fade));
                        }
                        if (transition.right_clip_id == clip.id &&
                            span.post_frames > 0) {
                            ExportColorFade fade{
                                span.cut_frame, span.post_frames, false, color};
                            for (int64_t index = span.pre_frames;
                                 index < span.frames(); ++index) {
                                fade.alphas.push_back(
                                    ColorVeilAlpha(span, index));
                            }
                            input.color_fades.push_back(std::move(fade));
                        }
                        continue;
                    }
                    if (transition.right_clip_id == clip.id) {
                        input.source_in = input.source_in.sub(pre);
                        input.timeline_in = input.timeline_in.sub(pre);
                        input.duration = input.duration.add(pre);
                        input.fade_in = transition.duration;
                    }
                    if (transition.left_clip_id == clip.id)
                        input.duration = input.duration.add(post);
                }
            } else if (audio) {
                // EF2 -- a cross fade reads the sound past the cut, as a
                // dissolve reads the picture: the chain starts `lead`
                // early and runs `tail` late.
                if (!audio_mix::ResolveClipAudioEdges(document, clip, track->id,
                                                      input.audio_edges, error))
                    return false;
                input.source_in = input.source_in.sub(input.audio_edges.lead);
                input.timeline_in =
                    input.timeline_in.sub(input.audio_edges.lead);
                input.duration = input.audio_edges.length;
            }
            // IA4 -- selection uses the chain's span, handles included.
            if (windowed) {
                const RationalTime inputEnd =
                    input.timeline_in.add(input.duration);
                const RationalTime reach = video ? windowStart : audioLead;
                if (!(inputEnd > reach) || !(input.timeline_in < windowStop)) {
                    continue;
                }
            }
            if (!motion) {
                if (!prepareSource(*source)) {
                    return false;
                }
                if (video && !sourceHasVideo[clip.source_id]) {
                    error = "audio-only source_id '" + clip.source_id +
                            "' cannot be exported from a video track";
                    return false;
                }
                if (audio && !sourceHasAudio[clip.source_id]) {
                    continue;
                }
            }
            // R3 -- after the transition handles, which are timeline time.
            input.source_duration = ScaleBySpeed(input.duration, clip.speed);
            // R3, criterion 4 -- a ramp is handed to the graph as the list
            // of frames the rule names (ClipSpeed.h), sampled here, once. A
            // dissolve is refused on a ramped clip (Document::Validate), so
            // its span is the clip's own.
            if (video && !motion && ClipHasSpeedRamp(clip)) {
                try {
                    input.listed_frames = ClipSourceFrames(
                        clip, 0,
                        ExactFrames(input.duration, settings.frame_rate,
                                    "ramped clip duration"),
                        settings.frame_rate, source->rate);
                } catch (const std::exception& exception) {
                    error = "clip '" + clip.id + "': " + exception.what();
                    return false;
                }
            }
            // VF1 -- docs/roadmap/VFR-2026-11.md. Which grid frames the
            // chain reads, by the rule (ClipSpeed.h) applied to the span the
            // chain covers, its dissolve handles included; then how FFmpeg
            // reads them on the source's grid, anchored on its first picture
            // as verify_export reads it (SourceGrid.h). Reading from where a
            // `-ss` landed made a whole clip one picture early whenever the
            // first picture after the seek came more than half a frame late,
            // as a phone's pictures do.
            if (video && !motion &&
                !planGridRead(input, clip, *source, error)) {
                return false;
            }
            // R1 -- one call, the same one the monitor, the sheets and
            // verify_export make.
            if (video && !motion && ClipHasFraming(clip)) {
                std::string framingError;
                if (!ResolveClipFramingForClip(document, clip, settings.width,
                                               settings.height, input.framing,
                                               framingError)) {
                    error = "clip '" + clip.id + "': " + framingError;
                    return false;
                }
                input.framed = true;
            }
            // R2 -- docs/roadmap/REEL-2026-09.md. One call per frame, the
            // same one the monitor, the sheets and verify_export make: the
            // values are sampled once, here, and the filter chain below
            // only spells out the list it was handed.
            // R2b -- docs/roadmap/REEL-2026-09.md. A motion clip takes
            // another path through this graph than the branch below, and its
            // keys used to be refused here while the monitor played them
            // (main.mm samples every clip that carries keys, motion
            // included): a fade to black on an end card was accepted, shown,
            // counted by check_timeline, and refused at the first frame of
            // the export. A motion clip only ever carries opacity keys
            // (SetClipAnimation refuses an animated framing on it), so its
            // opacity is sampled here by the rule every other clip follows,
            // one entry per frame of the chain, and its layer applies it as
            // it applies a constant opacity. Its picture is never framed: the
            // composition is laid out at the sequence size already. A key on
            // any other parameter -- a document written by hand -- is
            // refused, by the test SetClipAnimation makes: ClipHasFraming
            // reads the constant framing only, and let a zoom through.
            if (video && motion && ClipHasAnimation(clip)) {
                const bool framed =
                    std::any_of(clip.animation.begin(), clip.animation.end(),
                                [](const ClipParameterAnimation& entry) {
                                    return entry.parameter !=
                                           ClipAnimatedParameter::Opacity;
                                });
                if (framed) {
                    error = ClipAnimationRefusal(
                        clip,
                        "an export of a motion clip (only its opacity may "
                        "move; animate the composition's own nodes instead)");
                    return false;
                }
                std::string animationError;
                int64_t chainFrames = 0;
                int64_t headHandle = 0;
                try {
                    chainFrames =
                        ExactFrames(input.duration, settings.frame_rate,
                                    "animated motion clip duration");
                    headHandle =
                        ExactFrames(clip.timeline_in.sub(input.timeline_in),
                                    settings.frame_rate,
                                    "animated motion clip transition handle");
                } catch (const std::exception& exception) {
                    error = exception.what();
                    return false;
                }
                for (int64_t chainFrame = 0; chainFrame < chainFrames;
                     ++chainFrame) {
                    ClipSampledParameters sampled;
                    if (!SampleClipParameters(clip, chainFrame - headHandle,
                                              settings.frame_rate, sampled,
                                              animationError)) {
                        error = "clip '" + clip.id + "' at frame " +
                                std::to_string(chainFrame - headHandle) + ": " +
                                animationError;
                        return false;
                    }
                    input.opacity_by_frame.push_back(sampled.opacity);
                }
                input.animated_opacity = true;
            }
            if (video && !motion && ClipHasAnimation(clip)) {
                // One entry per frame of the CHAIN, not of the clip: a
                // dissolve's handles lengthen the chain before and after
                // the clip, and a list indexed from the handle's first frame
                // but sampled from the clip's put every value `pre` frames
                // early on a clip entering by a dissolve. The engine answers
                // the handles' frames, negative ones included, by the rule
                // ClipAnimation.h writes (they read the nearest key).
                std::string animationError;
                int64_t chainFrames = 0;
                int64_t headHandle = 0;
                try {
                    chainFrames =
                        ExactFrames(input.duration, settings.frame_rate,
                                    "animated clip duration");
                    headHandle = ExactFrames(
                        clip.timeline_in.sub(input.timeline_in),
                        settings.frame_rate, "animated clip transition handle");
                } catch (const std::exception& exception) {
                    error = exception.what();
                    return false;
                }
                for (int64_t chainFrame = 0; chainFrame < chainFrames;
                     ++chainFrame) {
                    const int64_t frame = chainFrame - headHandle;
                    ClipFramingGeometry geometry;
                    EffectParamValue opacity{1, 1};
                    if (!ResolveClipFramingAtFrame(document, clip, frame,
                                                   settings.width,
                                                   settings.height, geometry,
                                                   opacity, animationError)) {
                        error = "clip '" + clip.id + "' at frame " +
                                std::to_string(frame) + ": " + animationError;
                        return false;
                    }
                    input.framing_by_frame.push_back(geometry);
                    input.opacity_by_frame.push_back(opacity);
                }
                input.animated = true;
                input.framed = true;
                input.framing = input.framing_by_frame.front();
                input.animated_opacity =
                    ClipAnimates(clip, ClipAnimatedParameter::Opacity);
            }
            input.input_index = static_cast<int>(inputs.size());
            inputs.push_back(std::move(input));
        }
    }

    std::vector<std::string> arguments = {
        "-hide_banner", "-nostdin", "-y", "-progress", "pipe:1", "-nostats",
    };
    const std::string rate = std::to_string(settings.frame_rate.num) + "/" +
                             std::to_string(settings.frame_rate.den);
    // MOTION-2026-09 M4 -- each composition parsed once, and each motion
    // clip's frames decided here, at the export cadence, in exact time.
    std::map<Ulid, MotionComposition> parsedCompositions;
    for (const InputClip& input : inputs) {
        if (input.motion) {
            const DocumentComposition* stored =
                document.FindComposition(input.clip->composition_id);
            if (stored == nullptr) {
                error = "motion clip references an unknown composition";
                return false;
            }
            auto parsed = parsedCompositions.find(stored->id);
            if (parsed == parsedCompositions.end()) {
                MotionComposition composition;
                MotionError motionError;
                if (!ParseDocumentComposition(*stored, composition,
                                              motionError)) {
                    error = "composition '" + stored->id +
                            "' cannot be rendered: " + motionError.code + ": " +
                            motionError.message;
                    return false;
                }
                parsed =
                    parsedCompositions.emplace(stored->id, composition).first;
            }
            ExportMotionAsset asset;
            try {
                asset.frames = MotionClipFrames(
                    parsed->second, input.source_in,
                    ExactFrames(input.duration, settings.frame_rate,
                                "motion clip duration"),
                    settings.frame_rate);
            } catch (const std::exception& exception) {
                error = exception.what();
                return false;
            }
            asset.directory = (std::filesystem::temp_directory_path() /
                               ("cutmachine-motion-" + GenerateUlid()))
                                  .string();
            asset.composition = *stored;
            asset.props = input.clip->motion_props;
            asset.media_directory = baseDirectory.string();
            asset.render_scale =
                std::min({1.0,
                          static_cast<double>(settings.width) /
                              std::max(1, parsed->second.width),
                          static_cast<double>(settings.height) /
                              std::max(1, parsed->second.height)});
            // REM2 -- what the composition reaches, and whether it plays a
            // sound: refused here, before FFmpeg starts, when a media is
            // missing or a sound would play at another speed.
            MotionError motionError;
            if (!MotionReachFor(document, stored->id, asset.reach,
                                motionError)) {
                error = "composition '" + stored->id +
                        "': " + motionError.code + ": " + motionError.message;
                return false;
            }
            {
                MotionMediaMeasurer measurer(asset.media_directory,
                                             asset.reach.media_paths);
                std::string voices;
                size_t audible = 0;
                if (!DescribeMotionAudio(parsed->second, asset.props,
                                         asset.reach.Context(), measurer,
                                         voices, audible, motionError)) {
                    error =
                        "composition '" + stored->id +
                        "' sound: " + motionError.code +
                        (motionError.path.empty() ? ""
                                                  : " at " + motionError.path) +
                        ": " + motionError.message;
                    return false;
                }
                if (audible > 0) {
                    const auto sampleAt = [](const RationalTime& time) {
                        const __int128 numerator =
                            static_cast<__int128>(time.value) *
                            kMotionAudioRate;
                        return static_cast<int64_t>(
                            (numerator + time.rate / 2) / time.rate);
                    };
                    asset.audio = true;
                    asset.audio_first = sampleAt(input.source_in);
                    asset.audio_count = sampleAt(input.duration);
                    asset.audio_path = asset.directory + "/sound.wav";
                }
            }
            arguments.insert(arguments.end(), {"-f", "image2", "-framerate",
                                               rate, "-start_number", "0", "-i",
                                               asset.directory + "/%06d.png"});
            output.motion_assets.push_back(std::move(asset));
            continue;
        }
        // VF1 -- docs/roadmap/VFR-2026-11.md. A picture is read on the
        // source's grid (SourceGrid.h), whatever its speed: a seek a second
        // ahead of the first grid frame the chain needs, timestamps kept,
        // and the chain itself names the frames (`grid_read.filter`).
        if (input.video) {
            arguments.insert(arguments.end(),
                             input.grid_read.input_arguments.begin(),
                             input.grid_read.input_arguments.end());
            arguments.push_back("-i");
            arguments.push_back(
                ResolveMediaPath(baseDirectory, *input.source).string());
            continue;
        }
        // A sound reads `duration * speed` of its source (ClipSpeed.h),
        // from its source_in: the samples are placed by time, not on a grid.
        arguments.push_back("-ss");
        arguments.push_back(Decimal(input.source_in));
        arguments.push_back("-t");
        arguments.push_back(Decimal(input.source_duration));
        arguments.push_back("-i");
        arguments.push_back(
            ResolveMediaPath(baseDirectory, *input.source).string());
    }
    // REMOTION-PARITY-2026-09 REM2 -- the sound of each motion clip that
    // plays one, read after every clip's input so that none of their
    // indices moves: the clip, and the FFmpeg input of its mix.
    std::vector<std::pair<const InputClip*, int>> motionSounds;
    {
        size_t asset = 0;
        for (const InputClip& input : inputs) {
            if (!input.motion) {
                continue;
            }
            const ExportMotionAsset& motion = output.motion_assets[asset++];
            if (!motion.audio) {
                continue;
            }
            motionSounds.emplace_back(&input,
                                      static_cast<int>(CountInputs(arguments)));
            arguments.insert(arguments.end(), {"-i", motion.audio_path});
        }
    }

    // PA15 -- docs/roadmap/PARITE-2026-11.md. The matte of each clip cut
    // out along its people, read after every other input so that none of
    // their indices moves: frames from the first grid frame the clip's
    // chain reads (planGridRead, VF1), a few past the last it can ask for,
    // raw 8-bit grey at the media's rate. The chain below reads it through
    // the clip's own timing, crop and scale.
    std::map<const InputClip*, int> matteInputs;
    for (const InputClip& input : inputs) {
        if (input.motion || !input.video || input.clip == nullptr ||
            !ClipHasPersonMask(*input.clip))
            continue;
        std::string mattePath;
        person_matte::Info matte;
        if (!person_matte::MatteForMedia(document, baseDirectory.string(),
                                         input.clip->source_id, mattePath,
                                         matte, error)) {
            error = "clip '" + input.clip->id + "': " + error;
            return false;
        }
        const MediaRate sourceRate = input.source->rate;
        ExportMatteAsset asset;
        asset.matte_path = mattePath;
        asset.first_frame = input.grid_first;
        // Two frames past the last the rule asks for cost nothing: the
        // chain trims every branch to its frame count.
        asset.count = input.grid_last - input.grid_first + 1 + 2;
        asset.raw_path = (std::filesystem::temp_directory_path() /
                          ("cutmachine-matte-" + GenerateUlid() + ".gray"))
                             .string();
        matteInputs[&input] = static_cast<int>(CountInputs(arguments));
        arguments.insert(
            arguments.end(),
            {"-f", "rawvideo", "-pix_fmt", "gray", "-video_size",
             std::to_string(matte.width) + "x" + std::to_string(matte.height),
             "-framerate",
             std::to_string(sourceRate.num) + "/" +
                 std::to_string(sourceRate.den),
             "-i", asset.raw_path});
        output.matte_assets.push_back(std::move(asset));
    }

    std::ostringstream graph;
    graph.imbue(std::locale::classic());
    // VQA2 -- the colour layers of the dips (AppendColorVeil). They are
    // filtergraph sources, so each one is a chain of its own; they are
    // collected here and written in front of the composite below.
    std::ostringstream veilChains;
    veilChains.imbue(std::locale::classic());
    // F1.3 -- a colour-managed composite is scene-linear light in float;
    // black is zero there as in the display signal.
    const bool managed = document.color_management.enabled;
    // IA4 -- a window's black starts at its first frame, not at the film's:
    // every frame before it would only be composited to be dropped. The
    // frames keep the film's numbering (`setpts`), so every placement and
    // every `enable` below, which read the composite's time, are the ones
    // the full export runs.
    graph << "color=c=black:s=" << settings.width << 'x' << settings.height
          << ":r=" << rate
          << ":d=" << Decimal(windowed ? windowStop.sub(windowStart) : duration)
          << ",format=" << (managed ? "gbrapf32le" : "yuv444p10le");
    // Unmanaged, the black under every clip is BT.709 too (see the clip
    // chains below).
    if (!managed) graph << ",setparams=colorspace=bt709";
    if (windowFirst > 0) graph << ",setpts=PTS+" << windowFirst;
    graph << "[base0];";

    std::map<std::string, std::string> gradePaths;
    size_t videoOrdinal = 0;
    for (const InputClip& input : inputs) {
        if (!input.video) {
            continue;
        }
        int64_t firstFrame = 0;
        int64_t frameCount = 0;
        int64_t fadeFrames = 0;
        try {
            firstFrame = ExactFrames(input.timeline_in, settings.frame_rate,
                                     "video clip position");
            frameCount = ExactFrames(input.duration, settings.frame_rate,
                                     "video clip duration");
            if (input.fade_in) {
                fadeFrames = ExactFrames(*input.fade_in, settings.frame_rate,
                                         "transition duration");
            }
        } catch (const std::exception& exception) {
            error = exception.what();
            return false;
        }
        // R5 -- the dips over this clip, moved from sequence frames into
        // the chain's own. The unmanaged chain veils the picture below; the
        // managed one lays them in the SDR view (AppendManagedLayer).
        std::vector<ExportColorFade> localFades;
        localFades.reserve(input.color_fades.size());
        for (const ExportColorFade& fade : input.color_fades) {
            localFades.push_back(fade);
            localFades.back().start = fade.start - firstFrame;
        }
        // R1 -- the displayed size the crop below is expressed in, the
        // same one ClipFraming.h resolved the rectangle from.
        int64_t displayedWidth = 0;
        int64_t displayedHeight = 0;
        // PA8 -- docs/roadmap/PARITE-2026-11.md. A masked clip needs it
        // too: its mask is placed on the displayed source (ClipMask.h).
        const bool masked = !input.motion && ClipHasMask(*input.clip);
        if ((input.framed || masked) &&
            !ClipDisplayedSize(document, *input.clip, displayedWidth,
                               displayedHeight, error)) {
            return false;
        }
        // The part of the displayed source this chain's picture shows once
        // cropped and scaled, which each branch below names; the mask is
        // then one filter on that picture, after its blur and before it is
        // laid in the frame.
        ClipMaskRegion maskRegion{0,
                                  0,
                                  displayedWidth,
                                  displayedHeight,
                                  displayedWidth,
                                  displayedHeight};
        // EF3 -- docs/roadmap/EFFETS-2026-10.md. The clip's blur, carried
        // from sequence pixels into the delivery's (ClipBlur.h). Empty for
        // every clip that blurs nothing, whose chain keeps every byte.
        std::string blurFilter;
        if (!input.motion &&
            !ClipBlurFilter(*input.clip, settings.width, settings.height,
                            document.sequence.width, document.sequence.height,
                            blurFilter, error)) {
            return false;
        }
        graph << '[' << input.input_index << ":v:0]";
        // PA15 -- what follows, the clip's timing, crop and scale, is also
        // the way its matte is read when it is cut out along its people:
        // written once, here, and handed to both.
        std::ostringstream chain;
        chain.imbue(std::locale::classic());
        // Set for an animated clip: its per-frame scale, applied last.
        std::string animatedScale;
        // VF1 -- docs/roadmap/VFR-2026-11.md. Every picture is read on its
        // source's grid first (SourceGrid.h): anchored on the first
        // picture, resampled at the rate the source is counted at, and
        // trimmed to the first grid frame the chain needs, so the chain's
        // frame n is grid frame grid_first + n whatever the seek did. The
        // branches below then only count, from a first frame at time zero,
        // as every chain after this one expects (the managed layer's lead
        // frame and the `sendcmd` times of its veils among them). A motion
        // clip has no source: its rendered frames are already at the
        // sequence's cadence. This read is the picture's own, not part of
        // the chain its matte shares (PA15): the matte is handed to FFmpeg
        // from grid frame grid_first already, one frame per grid step.
        if (!input.motion) {
            graph << input.grid_read.filter << ",setpts=PTS-STARTPTS,";
        }
        // R3 -- docs/roadmap/REEL-2026-09.md. A constant speed is the
        // source read at another cadence, so that is exactly what the graph
        // says. `settb` + `setpts=N` give grid frame n the time n/R_eff,
        // R_eff being the source rate times the speed (ClipSpeed.h); and
        // `fps=<sequence rate>:round=up` picks, for output frame k, source
        // frame floor(k * R_eff / R_seq) -- the engine's rule, term for
        // term, which the frame-by-frame export test pins. Nothing here is
        // a formula FFmpeg evaluates in double: `setpts=N` is the integer
        // frame counter and the two cadences are exact rationals (ITW1).
        if (!input.listed_frames.empty()) {
            // R3, criterion 4 -- docs/roadmap/REEL-2026-09.md. A ramp is no
            // cadence `fps` can resample, so the graph is handed the list;
            // so is a clip at unit speed whose source is counted at another
            // rate than the sequence (VF1). The grid frames from the first
            // the list names to the last are numbered from zero:
            // `setpts=N`, then `PTS-STARTPTS`, which makes the number exact
            // whatever base N counts from on a given FFmpeg build. Frame n
            // then gets, as its time in sequence frames, the first output
            // frame that shows it or a later one -- min{k : list[k] >= n}
            // -- spelled as integer runs (FrameProgressionList), never as a
            // formula FFmpeg evaluates. `fps` at the sequence cadence keeps,
            // at each output frame k, the last source frame whose time is k
            // or earlier, which is list[k]: a frame the list skips shares
            // its time with the next one it shows and is dropped, and a
            // frame it holds is repeated until the next one's time. `tpad`
            // and `trim` then hold the clip to exactly its frame count,
            // whatever `fps` does at the end of its input.
            const int64_t first = input.listed_frames.front();
            const int64_t span = input.listed_frames.back() - first + 1;
            std::vector<int64_t> firstShown(static_cast<size_t>(span), 0);
            size_t output = 0;
            for (int64_t frame = 0; frame < span; ++frame) {
                while (input.listed_frames[output] < first + frame) {
                    ++output;
                }
                firstShown[static_cast<size_t>(frame)] =
                    static_cast<int64_t>(output);
            }
            chain << "trim=end_frame=" << span
                  << ",settb=" << settings.frame_rate.den << '/'
                  << settings.frame_rate.num
                  << ",setpts=N,setpts=PTS-STARTPTS,setpts='"
                  << FrameProgressionList(firstShown, "PTS")
                  << "',fps=fps=" << rate
                  << ",tpad=stop_mode=clone:stop=" << frameCount
                  << ",trim=end_frame=" << frameCount << ',';
        } else if (ClipIsFreeze(*input.clip)) {
            // R4 -- the frame the rule names is the chain's first: kept
            // alone, repeated to the clip's length, and counted at the
            // sequence cadence as every other branch leaves it.
            int64_t frames = 0;
            try {
                frames = ExactFrames(input.duration, settings.frame_rate,
                                     "freeze frame duration");
            } catch (const std::exception& exception) {
                error = exception.what();
                return false;
            }
            chain << "trim=end_frame=1,loop=loop=" << (frames - 1)
                  << ":size=1:start=0,settb=" << settings.frame_rate.den << '/'
                  << settings.frame_rate.num << ",setpts=N,";
        } else if (ClipHasSpeed(*input.clip)) {
            const MediaRate effective =
                SpeedAdjustedSourceRate(input.source->rate, input.clip->speed);
            chain << "settb=" << effective.den << '/' << effective.num
                  << ",setpts=N,fps=fps=" << rate
                  << ":round=up,trim=end_frame=" << frameCount << ',';
        } else if (input.motion) {
            chain << "fps=" << rate << ',';
        } else {
            // Unit speed, the source counted at the sequence's rate: one
            // grid frame per sequence frame (planGridRead).
            chain << "trim=end_frame=" << frameCount << ',';
        }
        // R1 -- docs/roadmap/REEL-2026-09.md. A framed clip reads the
        // source rectangle ClipFraming.h named and scales it to the
        // rectangle it named in the frame; the placement below puts it
        // there. Both are integers this file was handed, not expressions
        // FFmpeg evaluates: `crop` would otherwise round an odd offset down
        // to the chroma grid behind us, and `iw`/`ih` arithmetic would be a
        // second answer to the question ClipFraming.h answers (guard rail
        // 1). A clip that frames nothing keeps the single fitted scale this
        // graph has always had, byte for byte.
        if (input.animated) {
            // R2 -- docs/roadmap/REEL-2026-09.md. The crop is constant (R2
            // does not animate it, ClipAnimation.h says why), so it is the
            // one `crop` this chain needs -- and it is the rectangle
            // ClipFraming.h named, not one worked out here. What moves is
            // the size of the whole picture and where it is placed; the
            // frame does the cutting, which is what `overlay` does anyway,
            // so nothing has to be re-cropped per frame.
            const ClipFramingGeometry& first = input.framing_by_frame.front();
            if (first.crop_x != 0 || first.crop_y != 0 ||
                first.crop_width != displayedWidth ||
                first.crop_height != displayedHeight) {
                chain << "crop=" << first.crop_width << ':' << first.crop_height
                      << ':' << first.crop_x << ':' << first.crop_y << ',';
                maskRegion.x = first.crop_x;
                maskRegion.y = first.crop_y;
                maskRegion.width = first.crop_width;
                maskRegion.height = first.crop_height;
            }
            std::vector<int64_t> widths;
            std::vector<int64_t> heights;
            widths.reserve(input.framing_by_frame.size());
            heights.reserve(input.framing_by_frame.size());
            for (const ClipFramingGeometry& geometry : input.framing_by_frame) {
                widths.push_back(geometry.full_width);
                heights.push_back(geometry.full_height);
            }
            // VQA2 -- the per-frame size is applied last in this chain,
            // just before placement (see animatedScale below): every filter
            // after a size change keeps the size it was configured with, so
            // a grade, a dip or an opacity downstream of it cut the zoomed
            // picture to its first size, and `alphamerge` crashed FFmpeg.
            // The identity scale holds the chain's shape here.
            animatedScale = "scale=w='" + FrameValueList(widths, 0, "n-1") +
                            "':h='" + FrameValueList(heights, 0, "n-1") +
                            "':eval=frame:flags=lanczos";
            // EF3 -- a blurred clip animates its opacity only
            // (Document::Validate): its picture keeps one size, which it
            // takes here, where the blur below can be drawn at it. The
            // per-frame scale at the end of the chain then changes nothing.
            if (!blurFilter.empty()) {
                chain << "scale=" << first.full_width << ':'
                      << first.full_height << ":flags=lanczos";
            } else {
                chain << "scale=w=iw:h=ih";
            }
        } else if (input.framed) {
            const std::string crop = ClipFramingCropFilter(
                input.framing, displayedWidth, displayedHeight);
            if (!crop.empty()) {
                chain << crop << ',';
                maskRegion.x = input.framing.source_x;
                maskRegion.y = input.framing.source_y;
                maskRegion.width = input.framing.source_width;
                maskRegion.height = input.framing.source_height;
            }
            chain << "scale=" << input.framing.width << ':'
                  << input.framing.height << ":flags=lanczos";
        } else {
            chain << "scale=" << settings.width << ':' << settings.height
                  << ":force_original_aspect_ratio=decrease:flags=lanczos";
        }
        // MOTION-2026-09 M4 -- a motion frame is display-referred sRGB with
        // its own alpha. Unmanaged, it becomes the composite's limited-range
        // BT.709 at once, alpha kept, where the automatic conversion would
        // take the BT.601 matrix.
        // DEC31, BH2 -- docs/roadmap/BLANC-HLG-2026-10.md. A media declared
        // a graphic: its code values are what it shows, as a motion title's
        // are. An RGB one (a PNG) is converted with the BT.709 matrix the
        // delivery is tagged with, alpha kept, as a motion frame is. A
        // motion clip has no media (its source is null): never asked.
        graph << chain.str();
        // PA15 -- a clip cut out along its people takes its matte instead
        // of a shape's rule, as an alpha the picture did not have: it goes
        // where the picture is still opaque, before its opacity and its
        // fades scale that alpha.
        const bool personMasked = masked && ClipHasPersonMask(*input.clip) &&
                                  matteInputs.count(&input);
        std::string maskFilter;
        if (personMasked) {
            int64_t clipFrames = 0;
            try {
                clipFrames = ExactFrames(input.duration, settings.frame_rate,
                                         "masked clip duration");
            } catch (const std::exception& exception) {
                error = exception.what();
                return false;
            }
            maskFilter = person_matte::MatteFilter(
                matteInputs.at(&input), displayedWidth, displayedHeight,
                chain.str(), input.clip->mask.inverted,
                std::to_string(videoOrdinal), clipFrames);
        } else if (masked) {
            maskFilter = ClipMaskFilter(input.clip->mask, maskRegion,
                                        std::to_string(videoOrdinal));
        }
        const bool graphic =
            !input.motion && document.IsGraphicsMedia(input.source->id);
        if ((input.motion || (graphic && sourceIsRgb[input.source->id])) &&
            !managed) {
            graph << ":out_color_matrix=bt709:out_range=tv,format=yuva444p10le";
        }
        graph << ",setsar=1";
        // Unmanaged, every layer of the composite says BT.709, which is what
        // the delivery is tagged. An untagged picture is read as BT.601 by
        // swscale; a dip's veil and a motion clip are tagged BT.709; so a
        // dipped clip laid over an untagged one was converted from one
        // matrix to the other and laid over the black it was not, and the
        // choice depended on which clip lay underneath -- a window of the
        // film that started after the clip before it differed by 116 codes
        // in 1023 on saturated colours (segmented rendering found it). Only
        // the label is set: no pixel changes here.
        if (!managed && !input.motion && !sourceIsRgb[input.source->id]) {
            graph << ",setparams=colorspace=bt709";
        }
        // EF3 -- docs/roadmap/EFFETS-2026-10.md. The blur, on the picture
        // at its size in the frame and before it is laid there: its edges
        // are extended, never mixed with the frame's black or with the
        // transparent border the managed chain pads it with, as the monitor
        // blurs the rectangle it draws (Renderer.mm). Before the grade and
        // the colour transforms, in the picture's own signal, which is
        // where the monitor blurs it too: in 4:4:4 at 16 bits, alpha kept
        // (`gblur` gives a subsampled chroma plane the luma's sigma, which
        // would blur colour twice as wide). YCbCr is blurred as YCbCr: the
        // matrix to RGB is affine and the blur's weights sum to one, so it
        // is the same blur of the same RGB, with no conversion added here.
        if (!blurFilter.empty()) {
            const bool rgbStream =
                sourceIsRgb[input.source->id] && (managed || !graphic);
            graph << ",format=" << (rgbStream ? "gbrap16le" : "yuva444p16le")
                  << ',' << blurFilter;
        }
        // DEC31 -- under colour management a graphic is placed as authored,
        // in the graphics view, where no grade of this engine acts: a grade
        // on it would be dropped without a word, so the export says so.
        if (managed && graphic &&
            input.clip->effects.end() !=
                std::find_if(input.clip->effects.begin(),
                             input.clip->effects.end(),
                             [](const ClipEffect& effect) {
                                 return effect.type.rfind("color.", 0) == 0;
                             })) {
            error = "clip '" + input.clip->id +
                    "' is a graphic (its media's colour input is graphics): "
                    "under colour management it is placed as authored and "
                    "takes no grade; remove its color.* stages or read its "
                    "media as a camera's (set_media_color_input)";
            return false;
        }
        const ResolvedColorGrade grade = ResolveColorGrade(
            input.clip->effects, ColorGradeContextFor(document, *input.clip));
        // The display-referred assets a motion layer and a colour dip share
        // (OpenColorIoCompositeDisplayReferred): the inverse of the SDR
        // view, the view itself (and its shaper when grading in ACEScct),
        // and the unit cube that makes a coverage mask exact. Built once
        // per export, on first use.
        struct ViewAssets {
            std::string inverse;
            std::string view;
            std::string shaper;
            std::string coverage;
        };
        const auto viewAssets = [&](ViewAssets& assets) {
            // Its own cube: the inverse of the display view, so that a hex
            // colour comes out of the display transform as written
            // (ColorManagement.h). Every motion clip shares it.
            auto path = gradePaths.find("motion");
            if (path == gradePaths.end()) {
                ExportLutAsset asset;
                asset.path = (std::filesystem::temp_directory_path() /
                              ("cutmachine-motion-" + GenerateUlid() + ".cube"))
                                 .string();
                if (!BuildOpenColorIoDisplayReferredWorkingCube(
                        document.color_management, asset.cube, error)) {
                    error.insert(0,
                                 "unable to build the motion colour "
                                 "transform: ");
                    return false;
                }
                path = gradePaths.emplace("motion", asset.path).first;
                output.clip_grade_luts.push_back(std::move(asset));
            }
            // The view the motion cube inverts, forward, shared by every
            // motion clip of the export (OpenColorIoCompositeDisplayReferred):
            // the SDR view, or the HLG view in graphic code values (BH1).
            auto view = gradePaths.find("motion-view");
            if (view == gradePaths.end()) {
                std::string shaper;
                ExportLutAsset cube;
                if (!BuildOpenColorIoGraphicsViewCubes(
                        document.color_management, shaper, cube.cube, error)) {
                    error.insert(0,
                                 "unable to build the motion display "
                                 "view: ");
                    return false;
                }
                cube.path =
                    (std::filesystem::temp_directory_path() /
                     ("cutmachine-motion-view-" + GenerateUlid() + ".cube"))
                        .string();
                view = gradePaths.emplace("motion-view", cube.path).first;
                output.clip_grade_luts.push_back(std::move(cube));
                if (!shaper.empty()) {
                    ExportLutAsset shaperAsset;
                    shaperAsset.path = (std::filesystem::temp_directory_path() /
                                        ("cutmachine-motion-shaper-" +
                                         GenerateUlid() + ".cube"))
                                           .string();
                    shaperAsset.cube = std::move(shaper);
                    gradePaths.emplace("motion-view-shaper", shaperAsset.path);
                    output.clip_grade_luts.push_back(std::move(shaperAsset));
                }
            }
            auto coverage = gradePaths.find("motion-coverage");
            if (coverage == gradePaths.end()) {
                ExportLutAsset asset;
                asset.path =
                    (std::filesystem::temp_directory_path() /
                     ("cutmachine-coverage-" + GenerateUlid() + ".cube"))
                        .string();
                asset.cube = UnitCubeText();
                coverage =
                    gradePaths.emplace("motion-coverage", asset.path).first;
                output.clip_grade_luts.push_back(std::move(asset));
            }
            const auto viewShaper = gradePaths.find("motion-view-shaper");
            assets.inverse = path->second;
            assets.view = view->second;
            assets.coverage = coverage->second;
            assets.shaper =
                viewShaper != gradePaths.end() ? viewShaper->second : "";
            return true;
        };
        // R5 -- the dips over this clip, laid in the SDR view once the clip
        // is in the composite (AppendManagedLayer).
        const auto veilLayer = [&](ManagedLayer& layer) {
            if (localFades.empty()) return true;
            ViewAssets assets;
            if (!viewAssets(assets)) return false;
            layer.veils = localFades;
            layer.veil_view_shaper_path = assets.shaper;
            layer.veil_view_cube_path = assets.view;
            layer.veil_inverse_cube_path = assets.inverse;
            layer.coverage_cube_path = assets.coverage;
            return true;
        };
        if (managed && input.motion) {
            ViewAssets assets;
            if (!viewAssets(assets)) return false;
            ManagedLayer layer;
            layer.input_index = input.input_index;
            layer.ordinal = videoOrdinal;
            layer.first_frame = firstFrame;
            layer.frame_count = frameCount;
            layer.fade = input.fade_in.has_value();
            layer.fade_frames = fadeFrames;
            layer.opacity = input.clip->opacity;
            layer.rgb_source = true;
            layer.cube_path = assets.inverse;
            layer.display_referred = true;
            layer.view_cube_path = assets.view;
            layer.coverage_cube_path = assets.coverage;
            layer.framed = input.framed;
            layer.framing_x = input.framing.x;
            layer.framing_y = input.framing.y;
            layer.view_shaper_path = assets.shaper;
            // R2b -- the opacity it moves through, as for any other clip.
            if (input.animated_opacity) {
                layer.opacity_by_frame = input.opacity_by_frame;
            }
            if (!veilLayer(layer)) return false;
            AppendManagedLayer(graph, layer, document.color_management,
                               settings);
            videoOrdinal += 1 + layer.veils.size();
            continue;
        }
        if (managed) {
            // ITW5 then F1.3 -- ROADMAP.md. Each clip goes to the working
            // space through one cube, grade included, as fragment_working
            // does on the monitor; the display transform waits for the
            // composite. Identical grades, or none, share a cube. G3 -- from
            // its own media's input: clips of media read alike share it.
            // COLOR-T -- with the range and matrix of an untagged picture
            // named, as the monitor reads them, rather than left to swscale.
            const auto& tags = sourceTags[input.source->id];
            const ColorManagementSettings clipColor = ResolveUntaggedColorInput(
                document.ColorManagementFor(input.source->id), tags.first,
                tags.second);
            // DEC31 -- a graphic reaches the working space through the
            // inverse of the graphics view and is mixed in that view, as a
            // motion layer is (OpenColorIoCompositeDisplayReferred); its
            // YCbCr is read with its own matrix and range (clipColor).
            ViewAssets graphicAssets;
            if (graphic && !viewAssets(graphicAssets)) return false;
            const std::string key =
                "working:" + OpenColorIoCacheKey(clipColor) + ':' +
                ColorGradeKey(grade);
            auto path =
                graphic ? gradePaths.find("motion") : gradePaths.find(key);
            if (path == gradePaths.end()) {
                ExportLutAsset asset;
                asset.path =
                    (std::filesystem::temp_directory_path() /
                     ("cutmachine-working-" + GenerateUlid() + ".cube"))
                        .string();
                if (!BuildOpenColorIoWorkingCube(clipColor, grade, asset.cube,
                                                 error)) {
                    error.insert(0,
                                 "unable to build the working-space colour "
                                 "transform: ");
                    return false;
                }
                path = gradePaths.emplace(key, asset.path).first;
                output.clip_grade_luts.push_back(std::move(asset));
            }
            ManagedLayer layer;
            layer.input_index = input.input_index;
            layer.ordinal = videoOrdinal;
            layer.first_frame = firstFrame;
            layer.frame_count = frameCount;
            layer.fade = input.fade_in.has_value();
            layer.fade_frames = fadeFrames;
            layer.opacity = input.clip->opacity;
            layer.rgb_source = sourceIsRgb[input.source->id];
            layer.cube_path = path->second;
            layer.framed = input.framed;
            layer.framing_x = input.framing.x;
            layer.framing_y = input.framing.y;
            if (graphic) {
                layer.cube_path = graphicAssets.inverse;
                layer.display_referred = true;
                layer.view_cube_path = graphicAssets.view;
                layer.coverage_cube_path = graphicAssets.coverage;
                layer.view_shaper_path = graphicAssets.shaper;
            }
            if (input.animated) {
                // R2 -- docs/roadmap/REEL-2026-09.md. `pad` cannot place a
                // layer frame by frame: its expressions know the picture's
                // size but not the frame number. `overlay` can, and it is
                // how the unmanaged chain places the same clip -- so the
                // picture is put, at the per-frame size and position
                // ClipAnimation.h sampled, on a transparent canvas the
                // size of the frame, whose alpha then is the picture's
                // exact coverage (0 or 1, measured). What follows is the
                // managed layer every other clip gets. The canvas is
                // 10-bit, the most `overlay` blends: lossless for the
                // 10-bit camera files this lot measured, two bits short of
                // a 12-bit source. The format is set before the per-frame
                // scale, which no filter after it may change.
                std::vector<int64_t> xs;
                std::vector<int64_t> ys;
                xs.reserve(input.framing_by_frame.size());
                ys.reserve(input.framing_by_frame.size());
                for (const ClipFramingGeometry& geometry :
                     input.framing_by_frame) {
                    xs.push_back(geometry.full_x);
                    ys.push_back(geometry.full_y);
                }
                if (layer.rgb_source) {
                    graph << ",scale=out_range=full:out_color_matrix=bt709";
                }
                graph << ",format=yuva444p10le,";
                // PA8 -- the mask on the picture at its first size, before
                // the per-frame scale and before it is placed.
                if (!maskFilter.empty()) graph << maskFilter << ',';
                graph << animatedScale << ",setpts=PTS-STARTPTS[pic"
                      << videoOrdinal
                      << "];color=c=black@0:s=" << settings.width << 'x'
                      << settings.height << ":r=" << rate
                      << ":d=" << Decimal(input.duration)
                      << ",format=yuva444p10le[cnv" << videoOrdinal << "];[cnv"
                      << videoOrdinal << "][pic" << videoOrdinal
                      << "]overlay=format=yuv444p10:x='"
                      << FrameValueList(xs, 0, "n-1") << "':y='"
                      << FrameValueList(ys, 0, "n-1")
                      << "':eof_action=endall:shortest=1";
                layer.prepositioned = true;
                if (input.animated_opacity) {
                    layer.opacity_by_frame = input.opacity_by_frame;
                }
            }
            if (!layer.prepositioned) layer.mask_filter = maskFilter;
            if (!veilLayer(layer)) return false;
            AppendManagedLayer(graph, layer, clipColor, settings);
            videoOrdinal += 1 + layer.veils.size();
            continue;
        }
        if (grade.count > 0) {
            const std::string key = ColorGradeKey(grade);
            auto path = gradePaths.find(key);
            if (path == gradePaths.end()) {
                ExportLutAsset asset;
                asset.path = (std::filesystem::temp_directory_path() /
                              ("cutmachine-grade-" + GenerateUlid() + ".cube"))
                                 .string();
                if (!BuildColorGradeCube(grade, asset.cube, error)) {
                    return false;
                }
                path = gradePaths.emplace(key, asset.path).first;
                output.clip_grade_luts.push_back(std::move(asset));
            }
            graph << ",format=gbrp16le,lut3d=file='" << path->second
                  << "':interp=trilinear";
        }
        // R5 -- the veil goes over the graded picture, as it does in the
        // managed chain above and in the monitor: a fade to black is not a
        // thing the grade acts on.
        for (size_t index = 0; index < localFades.size(); ++index) {
            const ExportColorFade& fade = localFades[index];
            AppendColorVeil(veilChains, graph,
                            "veil" + std::to_string(videoOrdinal) + "_" +
                                std::to_string(index),
                            rate, input.duration, fade.start, fade.frames,
                            fade.to_color, fade.color);
        }
        const bool translucent =
            input.clip->opacity.num != input.clip->opacity.den;
        if ((input.fade_in || translucent || input.animated_opacity ||
             masked) &&
            !input.motion) {
            graph << ",format=yuva444p10le";
        }
        // PA15 -- the matte replaces an alpha that is still opaque here.
        if (personMasked) graph << ',' << maskFilter;
        if (translucent && !input.animated_opacity) {
            graph << ",colorchannelmixer=aa=(" << input.clip->opacity.num << '/'
                  << input.clip->opacity.den << ')';
        }
        // R2 -- docs/roadmap/REEL-2026-09.md. An animated opacity changes
        // every frame, and `colorchannelmixer` takes a number, not a curve:
        // `sendcmd` hands it the frame's value as that frame arrives, at the
        // clip's own frame times (hence the setpts). The picture is opaque
        // here, so scaling its alpha is exactly multiplying by the value.
        // R2b -- a motion clip arrives with its own alpha, its coverage:
        // scaled the same way, it is that coverage times the value, as a
        // constant opacity already makes it.
        //
        // VQA2 -- docs/roadmap/QA-AGENTIQUE-2026-09.md. This used to be a
        // generated 2x2 alpha blown up to the picture's per-frame size and
        // put on by `alphamerge`. On a clip whose zoom was animated too, the
        // picture changes size every frame, and FFmpeg 8.1 crashes in
        // alphamerge (SIGSEGV, SIGBUS or abort, six runs in six) while
        // either animation alone exports. The value no longer depends on
        // any size.
        if (input.animated_opacity) {
            std::ostringstream commands;
            commands.imbue(std::locale::classic());
            int64_t previous = -1;
            for (size_t frame = 0; frame < input.opacity_by_frame.size();
                 ++frame) {
                const EffectParamValue& value = input.opacity_by_frame[frame];
                // The one place an exact opacity becomes an 8-bit code: the
                // alpha this graph has always carried. Nearest, ties away
                // from zero, like every other rounding of this lot.
                int64_t code = value.den <= 0
                                   ? 255
                                   : (2LL * 255 * value.num + value.den) /
                                         (2LL * value.den);
                code = code < 0 ? 0 : (code > 255 ? 255 : code);
                if (code == previous) continue;
                previous = code;
                const RationalTime at{
                    static_cast<int64_t>(frame) * settings.frame_rate.den,
                    settings.frame_rate.num};
                commands << (commands.tellp() > 0 ? ";" : "") << Decimal(at)
                         << " colorchannelmixer@opacity" << videoOrdinal
                         << " aa " << code << "/255";
            }
            graph << ",setpts=PTS-STARTPTS,sendcmd=c='" << commands.str()
                  << "',colorchannelmixer@opacity" << videoOrdinal << "=aa=1";
        }
        if (input.fade_in) {
            // Counted in frames, like the placement: the fade starts on the
            // clip's first frame whatever timestamp the decoder gave it.
            graph << ",fade=t=in:s=0:n=" << fadeFrames << ":alpha=1";
        }
        // PA8 -- the mask, one more factor of the alpha the opacity and
        // the fade already scale, on the picture before its per-frame
        // scale and its place in the frame.
        if (!maskFilter.empty() && !personMasked) graph << ',' << maskFilter;
        if (!animatedScale.empty()) graph << ',' << animatedScale;
        // After fps= the time base is exactly 1/rate, so the offset is an
        // integer number of time-base units and the sum stays exact.
        // DEC28 -- docs/roadmap/REEL-2026-09.md. The unmanaged composite
        // is 4:4:4 at 10 bits, as its black canvas is and as the managed
        // one composes: `overlay` left to its default format composed in
        // 4:2:0 and placed an edge on an odd row or column one pixel up or
        // left of where the monitor draws it (VISUAL_QA_CHECKLIST.md, box
        // 161). The delivery format is reached once, at the end of the
        // chain. Decided by the author on 23 October 2026, at the price of
        // bytes that change for every unmanaged export.
        graph << ",setpts=PTS-STARTPTS+" << firstFrame << "[v" << videoOrdinal
              << "];" << "[base" << videoOrdinal << "][v" << videoOrdinal
              << "]overlay=format=yuv444p10:";
        // R1 -- a framed clip is placed at the integers ClipFraming.h
        // named; an unframed one keeps the centring expression this graph
        // has always had. R2 -- an animated one is placed at the list
        // ClipAnimation.h named, one entry per timeline frame. `overlay`
        // counts the base's frames, so the index is the timeline frame,
        // and `n - 1` for the reason FrameValueList measures.
        if (input.animated) {
            std::vector<int64_t> xs;
            std::vector<int64_t> ys;
            xs.reserve(input.framing_by_frame.size());
            ys.reserve(input.framing_by_frame.size());
            for (const ClipFramingGeometry& geometry : input.framing_by_frame) {
                xs.push_back(geometry.full_x);
                ys.push_back(geometry.full_y);
            }
            // IA4 -- `overlay` counts the composite's frames, which start
            // at the window's first frame.
            graph << "x='"
                  << FrameValueList(xs, firstFrame - windowFirst, "n-1")
                  << "':y='"
                  << FrameValueList(ys, firstFrame - windowFirst, "n-1")
                  << '\'';
        } else if (input.framed) {
            graph << "x=" << input.framing.x << ":y=" << input.framing.y;
        } else {
            graph << "x=(W-w)/2:y=(H-h)/2";
        }
        graph << ":eof_action=repeat:repeatlast=1:shortest=0:enable="
              << FrameWindow(firstFrame, firstFrame + frameCount,
                             settings.frame_rate)
              << "[base" << (videoOrdinal + 1) << "];";
        ++videoOrdinal;
    }
    graph << "[base" << videoOrdinal << ']';
    // R6 -- docs/roadmap/REEL-2026-09.md. Without colour management the
    // composite is already the delivered signal, so the look goes on it
    // straight, in full-range RGB as the per-clip grades do
    // (BuildColorGradeCube's lut3d). It is still the same place in the
    // chain: after every clip's grade, before the delivery format and the
    // captions, which are drawn on the picture that is shipped.
    if (look.stage == LookLutStage::Display) {
        graph << "format=gbrp16le,lut3d=file='" << look.path
              << "':interp=trilinear,";
    }
    if (managed) {
        // F1.3 -- the display transform, once, on the linear composite, then
        // straight to the delivery format so captions draw on 10 bits.
        std::string shaper;
        if (!BuildOpenColorIoDisplayCubes(document.color_management, shaper,
                                          output.color_lut, error)) {
            error = "unable to build the display colour transform: " + error;
            return false;
        }
        output.temporary_lut_path =
            (std::filesystem::temp_directory_path() /
             ("cutmachine-display-" + GenerateUlid() + ".cube"))
                .string();
        if (!shaper.empty()) {
            output.display_shaper.path =
                (std::filesystem::temp_directory_path() /
                 ("cutmachine-shaper-" + GenerateUlid() + ".cube"))
                    .string();
            output.display_shaper.cube = std::move(shaper);
            graph << "lut1d=file='" << output.display_shaper.path
                  << "':interp=linear,";
        }
        // R6 -- and here the look, on the ACEScct code values the shaper
        // has just produced and the display cube is about to read: after
        // every clip's grade, after the composite, before the output
        // transform. Exactly the stage the survey of 19 September named,
        // and the only one under colour management where a .cube's 0..1
        // domain means anything.
        if (look.stage == LookLutStage::WorkingLog) {
            graph << "lut3d=file='" << look.path << "':interp=trilinear,";
        }
        graph << "lut3d=file='" << output.temporary_lut_path
              << "':interp=trilinear,";
        // R6 -- a look declared on the delivered Rec.709 signal, laid on
        // it: after the output transform, before the delivery format and
        // the captions, where the monitor's displaySignalWithLook lays it.
        // lut3d clamps its input to the cube, as the monitor clamps the
        // display signal before sampling.
        if (look.stage == LookLutStage::DeliveredDisplay) {
            graph << "lut3d=file='" << look.path << "':interp=trilinear,";
        }
        graph << "scale=out_range=tv:out_color_matrix="
              << (document.color_management.output_gamut == "rec2020" ? "bt2020"
                                                                      : "bt709")
              << ",format=" << (settings.main10 ? "yuv420p10le" : "yuv420p")
              << ',';
    }
    // ADOBE-CLI-2026-09 -- caption colors belong to the delivered image,
    // after the source display transform, as they do in the monitor overlay.
    // LG3 -- docs/roadmap/LEGENDES-2026-10.md. The cartons go on as one
    // layer the caption renderer draws (ExportCaptionAsset), where drawtext
    // drew them. A carton shows on every frame that starts inside its
    // interval -- a caption imported from SRT need not sit on a frame
    // boundary -- and a window draws only the cartons it shows. Every
    // carton's style is checked, shown or not, as before.
    ExportCaptionAsset captions;
    struct CartonSpan {
        size_t carton = 0;
        int64_t first = 0;
        int64_t end = 0;
        // PA4 -- the clip, when its style animates words it has times for.
        const DocumentClip* animated = nullptr;
    };
    std::vector<CartonSpan> spans;
    for (const auto* track : tracks) {
        if (track->kind != "caption" || !track->visible) {
            continue;
        }
        for (const auto& clip : track->clips) {
            // A disabled carton is not burned (ClipEnabled.h).
            if (clip.caption_text.empty() ||
                !ClipIsSeen(document.sequence, clip)) {
                continue;
            }
            CaptionStyle style;
            if (!clip.caption_group_id.empty()) {
                const auto* stored =
                    document.FindCaptionStyle(clip.caption_group_id);
                if (stored == nullptr) {
                    error = "caption references an unknown style";
                    return false;
                }
                style = *stored;
            }
            const auto validColor = [](const std::string& color) {
                std::array<uint8_t, 4> rgba{};
                return ParseCaptionColor(color, rgba);
            };
            if (!validColor(style.color) || style.font_family.empty() ||
                style.font_family.find('\0') != std::string::npos ||
                style.font_size <= 0 ||
                clip.caption_text.find('\0') != std::string::npos ||
                (style.position != "top" && style.position != "center" &&
                 style.position != "bottom") ||
                (style.outline_width > 0 && !validColor(style.outline_color)) ||
                (style.shadow_offset > 0 && !validColor(style.shadow_color)) ||
                (!style.background_color.empty() &&
                 !validColor(style.background_color))) {
                error = "unsupported caption font, color, position or NUL text";
                return false;
            }
            CartonSpan span;
            try {
                span.first = CeilFrames(clip.timeline_in, settings.frame_rate);
                span.end = CeilFrames(clip.timeline_in.add(clip.duration),
                                      settings.frame_rate);
            } catch (const std::exception& exception) {
                error = exception.what();
                return false;
            }
            span.first = std::max(span.first, windowFirst);
            span.end = std::min(span.end, windowEnd);
            if (span.first >= span.end) {
                continue;
            }
            span.carton = captions.cartons.size();
            if (!style.word_animation.empty() &&
                !clip.caption_word_times.empty())
                span.animated = &clip;
            spans.push_back(span);
            captions.cartons.push_back(
                {clip.caption_text, style, clip.opacity});
        }
    }
    if (!spans.empty()) {
        // Each frame shows the cartons on screen at once, in track order;
        // each distinct set is one picture, drawn once.
        // PA4 -- a carton whose words are animated is a state per word:
        // the frame's picture is its cartons and how many words each has
        // started when the frame begins.
        using Shown = std::vector<std::pair<size_t, int32_t>>;
        std::vector<Shown> onScreen(
            static_cast<size_t>(windowEnd - windowFirst));
        try {
            for (const CartonSpan& span : spans) {
                for (int64_t frame = span.first; frame < span.end; ++frame) {
                    const int32_t spoken =
                        span.animated == nullptr
                            ? -1
                            : CaptionWordsSpoken(
                                  *span.animated,
                                  RationalTime{frame * settings.frame_rate.den,
                                               settings.frame_rate.num});
                    onScreen[static_cast<size_t>(frame - windowFirst)]
                        .emplace_back(span.carton, spoken);
                }
            }
        } catch (const std::exception& exception) {
            error = exception.what();
            return false;
        }
        std::map<Shown, uint32_t> pictureOf{{{}, 0}};
        captions.pictures = {{}};
        captions.picture_words = {{}};
        captions.frames.reserve(onScreen.size());
        for (const Shown& shown : onScreen) {
            const auto [found, added] = pictureOf.emplace(
                shown, static_cast<uint32_t>(captions.pictures.size()));
            if (added) {
                std::vector<size_t> cartons;
                std::vector<int32_t> words;
                for (const auto& [carton, spoken] : shown) {
                    cartons.push_back(carton);
                    words.push_back(spoken);
                }
                captions.pictures.push_back(std::move(cartons));
                captions.picture_words.push_back(std::move(words));
            }
            captions.frames.push_back(found->second);
        }
        captions.directory = (std::filesystem::temp_directory_path() /
                              ("cutmachine-captions-" + GenerateUlid()))
                                 .string();
        captions.width = settings.width;
        captions.height = settings.height;
        captions.sequence_width = document.sequence.width;
        const size_t input = CountInputs(arguments);
        arguments.insert(arguments.end(),
                         {"-f", "image2", "-framerate", rate, "-start_number",
                          "0", "-i", captions.directory + "/%06d.png"});
        // The composite goes into the delivered format first -- the
        // conversion the end of this chain makes anyway -- and the cartons
        // are blended in it: a pixel outside them keeps the bytes the film
        // has without captions, whatever format the composite reached them
        // in (10-bit 4:4:4 out of the clips' `overlay` since DEC28, or out
        // of the managed chain, 16-bit RGB out of a look),
        // so a window cut beside a carton is the film's frame for frame.
        const char* delivered = settings.main10 ? "yuv420p10le" : "yuv420p";
        graph << "format=" << delivered << ",null[captionbase];[" << input
              << ":v]setpts=PTS-STARTPTS+" << windowFirst
              << ",format=rgba,scale=";
        // Into the composite's matrix and range, as drawtext converted a
        // colour: a caption colour lands on the code value it always had.
        // Out of a look in RGB the composite takes the default matrix of
        // that conversion; the cartons take the same.
        if (managed) {
            graph << "out_color_matrix="
                  << (document.color_management.output_gamut == "rec2020"
                          ? "bt2020"
                          : "bt709")
                  << ':';
        } else if (look.stage != LookLutStage::Display) {
            graph << "out_color_matrix=bt709:";
        }
        graph << "out_range=tv,format=yuva444p10le[captions];[captionbase]"
                 "[captions]overlay=format="
              << (settings.main10 ? "yuv420p10" : "yuv420")
              << ":eof_action=pass:shortest=0[captioned];[captioned]";
        output.caption_assets.push_back(std::move(captions));
    }
    graph << "format=" << (settings.main10 ? "yuv420p10le" : "yuv420p")
          << ",setparams=range=limited:color_primaries="
          << (document.color_management.enabled &&
                      document.color_management.output_gamut == "rec2020"
                  ? "bt2020"
                  : "bt709")
          << ":color_trc="
          << (document.color_management.enabled &&
                      document.color_management.output_transfer == "hlg"
                  ? "arib-std-b67"
                  : (settings.sdr_tag == ExportSdrTag::Rec709A ? "unknown"
                                                               : "bt709"))
          << ":colorspace="
          << (document.color_management.enabled &&
                      document.color_management.output_gamut == "rec2020"
                  ? "bt2020nc"
                  : "bt709");
    // IA4 -- the window starts its own clock once every caption has been
    // drawn on the film's.
    if (windowFirst > 0) graph << ",setpts=PTS-STARTPTS";
    // IA4 -- the frames an inspection keeps, counted from the window's
    // first: `select` numbers the frames it receives from zero.
    if (!settings.inspection_frames.empty()) {
        graph << ",select=";
        for (size_t index = 0; index < settings.inspection_frames.size();
             ++index) {
            graph << (index ? "+" : "") << "eq(n\\,"
                  << (settings.inspection_frames[index] - windowFirst) << ')';
        }
    }
    graph << "[video];";

    size_t audioOrdinal = 0;
    for (const InputClip& input : inputs) {
        if (!input.audio) continue;
        const __int128 sampleNumerator =
            static_cast<__int128>(input.timeline_in.value) *
            settings.audio_sample_rate;
        const __int128 sampleDenominator = input.timeline_in.rate;
        const int64_t delaySamples = static_cast<int64_t>(
            (sampleNumerator + sampleDenominator / 2) / sampleDenominator);
        // R3 -- criterion 2. A linked clip's sound follows its picture:
        // `atrim` takes the source span the speed asks for and `atempo`
        // stretches it back to the clip's timeline duration, pitch kept.
        // The chain's factors multiply to the speed exactly (ClipSpeed.h),
        // so the sound cannot drift away from the image over a long shot.
        graph << '[' << input.input_index
              << ":a:0]atrim=duration=" << Decimal(input.source_duration)
              << ",asetpts=PTS-STARTPTS" << SpeedAtempoChain(input.clip->speed)
              << ",aresample=" << settings.audio_sample_rate
              << ",aformat=sample_fmts=fltp:channel_layouts=stereo";
        audio_mix::AppendClipEnvelope(graph, *input.clip, input.audio_edges);
        graph << ",adelay=" << delaySamples << "S:all=1[a" << audioOrdinal
              << "];";
        ++audioOrdinal;
    }
    // REM2 -- a motion clip's sound: its composition's mix over the part it
    // shows, already cut to the clip, placed where the clip starts, with the
    // 3 ms edges every clip of the mix gets.
    for (const auto& [input, index] : motionSounds) {
        const __int128 sampleNumerator =
            static_cast<__int128>(input->timeline_in.value) *
            settings.audio_sample_rate;
        const __int128 sampleDenominator = input->timeline_in.rate;
        const int64_t delaySamples = static_cast<int64_t>(
            (sampleNumerator + sampleDenominator / 2) / sampleDenominator);
        graph << '[' << index << ":a:0]aresample=" << settings.audio_sample_rate
              << ",aformat=sample_fmts=fltp:channel_layouts=stereo";
        audio_mix::AppendClipEnvelope(graph, *input->clip);
        graph << ",adelay=" << delaySamples << "S:all=1[a" << audioOrdinal
              << "];";
        ++audioOrdinal;
    }
    if (audioOrdinal == 0) {
        graph << "anullsrc=r=" << settings.audio_sample_rate << ":cl=stereo:d="
              << Decimal(windowed ? windowStop.sub(windowStart) : duration)
              << "[audio]";
    } else if (windowFirst == 0) {
        for (size_t index = 0; index < audioOrdinal; ++index)
            graph << "[a" << index << ']';
        audio_mix::AppendMixedOutput(graph, audioOrdinal,
                                     windowed ? windowStop : duration);
    } else {
        // IA4 -- the mix runs from the film's zero, limiter included, and
        // the window is cut out of it at the sample: the placement above
        // rounds a clip's start to the nearest sample, so does this.
        for (size_t index = 0; index < audioOrdinal; ++index)
            graph << "[a" << index << ']';
        std::ostringstream mixed;
        audio_mix::AppendMixedOutput(mixed, audioOrdinal, windowStop);
        std::string mixedChain = mixed.str();
        mixedChain.erase(mixedChain.size() - std::string("[audio]").size());
        const auto sampleAt = [&](const RationalTime& time) {
            const __int128 numerator =
                static_cast<__int128>(time.value) * settings.audio_sample_rate;
            return static_cast<int64_t>((numerator + (time.rate / 2)) /
                                        time.rate);
        };
        graph << mixedChain << ",atrim=start_sample=" << sampleAt(windowStart)
              << ":end_sample=" << sampleAt(windowStop)
              << ",asetpts=PTS-STARTPTS[audio]";
    }

    // R2 -- the generated alpha planes go in front: a filtergraph source
    // has to start its own chain, and labels resolve across the whole
    // string whatever the order. Empty for every export that animates no
    // opacity, so the graph such an export sends is byte for byte the one
    // it sent before this ticket.
    const std::string filterGraph = veilChains.str() + graph.str();
    arguments.insert(arguments.end(), {"-filter_complex", filterGraph, "-map",
                                       "[video]", "-map", "[audio]"});
    if (!AppendEncodingArguments(document, settings, arguments, error)) {
        return false;
    }

    output.settings = settings;
    output.timeline_id = document.sequence.id;
    output.settings.output_path = destination.string();
    // PA2 -- a delivery with a target carries where its run reports; an
    // inspection render (a window, a segment) is levelled by nobody.
    if (settings.loudness.requested && !inspection)
        output.loudness_outcome = std::make_shared<ExportLoudnessOutcome>();
    output.duration = windowed ? windowStop.sub(windowStart) : duration;
    try {
        output.total_frames = ExactFrames(output.duration, settings.frame_rate);
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
    // ADOBE-CLI-2026-09 -- muxers otherwise infer their cadence from a
    // source input, and lavfi duration can emit one extra boundary frame.
    if (settings.inspection_frames.empty()) {
        arguments.insert(
            arguments.end(),
            {"-r", rate, "-frames:v", std::to_string(output.total_frames)});
    } else {
        // IA4 -- the kept frames pass as they are: a constant cadence would
        // fill the holes `select` leaves with duplicates (R8 measured it).
        // No `-frames:v`: `select` already keeps exactly these frames, and a
        // frame count ended the file at the last one kept, cutting the sound
        // there -- the window's sound is kept whole (see ExportSettings).
        arguments.insert(arguments.end(), {"-fps_mode", "passthrough"});
    }
    output.filter_graph = filterGraph;
    output.temporary_output_path = TemporaryOutputPath(destination);
    arguments.push_back(output.temporary_output_path);
    output.arguments = std::move(arguments);
    if (!windowed && settings.inspection_frames.empty()) {
        const size_t limit =
            settings.max_inputs_per_pass > 0
                ? static_cast<size_t>(settings.max_inputs_per_pass)
                : DefaultInputsPerPass(document);
        if (CountInputs(output.arguments) > limit &&
            !BuildSegments(document, documentPath, settings, limit, output,
                           error)) {
            return false;
        }
    }
    // Decided on 8 October 2026 (ROADMAP.md): a caption whose family the
    // caption renderer lacks goes out in another one, said rather than
    // refused. Asked once, for the delivery: its segments and inspect_cut
    // windows are inspection renders.
    if (!inspection) {
        std::vector<std::string> families;
        const auto collect = [&families](const ExportPlan& plan) {
            for (const ExportCaptionAsset& asset : plan.caption_assets) {
                for (const CaptionCarton& carton : asset.cartons) {
                    const std::string family = CaptionFontName(carton.style);
                    if (std::find(families.begin(), families.end(), family) ==
                        families.end()) {
                        families.push_back(family);
                    }
                }
            }
        };
        collect(output);
        for (const ExportPlan& segment : output.segments) {
            collect(segment);
        }
        output.caption_font_substitutions = CheckCaptionFonts(families);
    }
    return true;
}

namespace {

// PA2 -- docs/roadmap/PARITE-2026-11.md. Brings the rendered file at
// plan.temporary_output_path to the delivery's loudness (see
// ExportLoudnessTarget): measured, one gain, the mix's limiter again, the
// sound encoded once and the picture copied. The result takes the rendered
// file's place, so the commit that follows is the one every export makes.
bool ApplyLoudnessTarget(const ExportPlan& plan, const std::atomic_bool* cancel,
                         std::string& error) {
    ExportLoudnessOutcome& outcome = *plan.loudness_outcome;
    outcome = {};
    const ExportSettings& settings = plan.settings;
    outcome.target_lufs = settings.loudness.integrated_millilufs / 1000.0;
    const std::string& rendered = plan.temporary_output_path;

    FileLoudness mix;
    if (!MeasureFileLoudness(rendered, settings.ffmpeg_path, mix, error))
        return false;
    outcome.mix_lufs = mix.integrated_lufs;
    outcome.mix_true_peak_dbfs = mix.true_peak_dbfs;
    outcome.silent = !mix.integrated_lufs.has_value();
    outcome.gain_db =
        outcome.silent ? 0.0 : outcome.target_lufs - *mix.integrated_lufs;
    if (cancel != nullptr && cancel->load()) {
        error = "export cancelled";
        return false;
    }

    const std::filesystem::path source(rendered);
    const std::string levelled =
        (source.parent_path() /
         (source.stem().string() + "-level" + source.extension().string()))
            .string();
    // The limiter is the mix's own (AppendMixedOutput): a gain upwards can
    // lift a peak the first one had held.
    std::vector<std::string> arguments = {
        settings.ffmpeg_path,
        "-hide_banner",
        "-nostdin",
        "-y",
        "-loglevel",
        "error",
        "-i",
        rendered,
        "-map",
        "0:v:0",
        "-map",
        "0:a:0",
        "-c:v",
        "copy",
        "-tag:v",
        Exporter::IsH264(settings.encoder) ? "avc1" : "hvc1",
        "-af",
        "volume=" + LoudnessDecimal(outcome.gain_db) +
            "dB,alimiter=limit=0.668344:level=0:latency=1",
        "-c:a",
        "aac",
        "-b:a",
        std::to_string(settings.audio_bitrate),
        "-ar",
        std::to_string(settings.audio_sample_rate),
        "-movflags",
        settings.sdr_tag == ExportSdrTag::Rec709A
            ? "+faststart+write_colr+write_gama"
            : "+faststart"};
    if (settings.sdr_tag == ExportSdrTag::Rec709A)
        arguments.insert(arguments.end(), {"-mov_gamma", "2.4"});
    arguments.push_back(levelled);
    std::string diagnostics;
    const bool encoded = ReadFfmpegRawFrames(
        arguments, 1, [](const uint8_t*) {}, error, &diagnostics);
    std::error_code moveError;
    if (encoded) std::filesystem::rename(levelled, rendered, moveError);
    if (!encoded || moveError) {
        std::error_code ignored;
        std::filesystem::remove(levelled, ignored);
        error = "unable to bring the export to its loudness: " +
                (encoded ? moveError.message() : error);
        return false;
    }

    FileLoudness delivered;
    if (!MeasureFileLoudness(rendered, settings.ffmpeg_path, delivered, error))
        return false;
    outcome.delivered_lufs = delivered.integrated_lufs;
    outcome.delivered_true_peak_dbfs = delivered.true_peak_dbfs;
    outcome.applied = true;
    return true;
}

}  // namespace

bool Exporter::Run(const ExportPlan& plan,
                   const ExportProgressCallback& callback,
                   const std::atomic_bool* cancel, std::string& error) {
    error.clear();
    // PRD2 -- an encoder or a filter this FFmpeg lacks is refused before
    // anything is rendered: the launch would refuse it too, but only after
    // the motion frames were drawn and the segments encoded.
    if (!RequireCapabilities(plan, error)) {
        return false;
    }
    if (!plan.segments.empty()) {
        return RunSegments(plan, callback, cancel, error);
    }
    const auto removeTemporaryFiles = [&]() {
        std::error_code ignored;
        std::filesystem::remove(plan.temporary_output_path, ignored);
        if (!plan.temporary_lut_path.empty()) {
            std::filesystem::remove(plan.temporary_lut_path, ignored);
        }
        for (const auto& asset : plan.clip_grade_luts) {
            std::filesystem::remove(asset.path, ignored);
        }
        if (!plan.display_shaper.path.empty()) {
            std::filesystem::remove(plan.display_shaper.path, ignored);
        }
        for (const auto& asset : plan.caption_assets) {
            std::filesystem::remove_all(asset.directory, ignored);
        }
        for (const auto& asset : plan.motion_assets) {
            std::filesystem::remove_all(asset.directory, ignored);
        }
        for (const auto& asset : plan.matte_assets) {
            std::filesystem::remove(asset.raw_path, ignored);
        }
    };
    // PA15 -- the mattes of the clips cut out along their people, raw,
    // before FFmpeg reads them.
    for (const ExportMatteAsset& asset : plan.matte_assets) {
        if (!person_matte::WriteRawFrames(asset.matte_path, asset.first_frame,
                                          asset.count, asset.raw_path, error)) {
            error = "the matte of a clip cut out along its people: " + error;
            removeTemporaryFiles();
            return false;
        }
    }
    // MOTION-2026-09 M4 -- the motion clips' frames, before FFmpeg reads
    // them. A missing font or image is the renderer's named refusal.
    for (const ExportMotionAsset& asset : plan.motion_assets) {
        if (!RenderMotionAsset(asset, cancel, error)) {
            removeTemporaryFiles();
            return false;
        }
    }
    // LG3 -- the cartons' layer, before FFmpeg reads it.
    for (const ExportCaptionAsset& asset : plan.caption_assets) {
        if (!RenderCaptionAsset(asset, cancel, error)) {
            removeTemporaryFiles();
            return false;
        }
    }
    for (const auto& asset : plan.clip_grade_luts) {
        std::ofstream lut(asset.path, std::ios::binary | std::ios::trunc);
        lut << asset.cube;
        lut.close();
        if (!lut) {
            error = "unable to create the export clip grade LUT";
            removeTemporaryFiles();
            return false;
        }
    }
    if (!plan.display_shaper.path.empty()) {
        std::ofstream lut(plan.display_shaper.path,
                          std::ios::binary | std::ios::trunc);
        lut << plan.display_shaper.cube;
        lut.close();
        if (!lut) {
            error = "unable to create the export display shaper";
            removeTemporaryFiles();
            return false;
        }
    }
    if (!plan.temporary_lut_path.empty()) {
        std::ofstream lut(plan.temporary_lut_path,
                          std::ios::binary | std::ios::trunc);
        lut << plan.color_lut;
        lut.close();
        if (!lut) {
            error = "unable to create the export color transform";
            removeTemporaryFiles();
            return false;
        }
    }
    // PRD1 -- settled here rather than left to the launcher, so that the
    // command file below names the binary that actually runs.
    std::string binary;
    if (!ffmpeg::Executable(plan.settings.ffmpeg_path, binary, error)) {
        removeTemporaryFiles();
        return false;
    }
    std::vector<std::string> storage;
    storage.reserve(plan.arguments.size() + 1);
    storage.push_back(binary);
    storage.insert(storage.end(), plan.arguments.begin(), plan.arguments.end());
    // Opt-in, for whoever has to reproduce a failed export by hand: the
    // exact command, one shell-quoted argument per line.
    if (const char* commandFile = std::getenv("CUTMACHINE_EXPORT_COMMAND_FILE");
        commandFile != nullptr && commandFile[0] != '\0') {
        std::ofstream command(commandFile, std::ios::trunc);
        for (const std::string& value : storage) {
            std::string quoted = "'";
            for (const char c : value) {
                quoted += c == '\'' ? std::string("'\\''") : std::string(1, c);
            }
            command << quoted << "' \\\n";
        }
    }

    ExportProgress current;
    current.total_frames = plan.total_frames;
    std::string progressBuffer;
    process::Request request;
    request.arguments = std::move(storage);
    request.standard_output = process::Stream::kPipe;
    request.standard_error = process::Stream::kPipe;
    request.read_bytes = 4096;
    request.error_tail_bytes = 32768;
    request.cancelled = [cancel]() {
        return cancel != nullptr && cancel->load();
    };
    request.on_output = [&](const char* bytes, size_t count) {
        progressBuffer.append(bytes, count);
        size_t newline = 0;
        while ((newline = progressBuffer.find('\n')) != std::string::npos) {
            const std::string line = progressBuffer.substr(0, newline);
            progressBuffer.erase(0, newline + 1);
            if (ParseProgressLine(line, current) && callback) {
                callback(current);
            }
        }
    };
    const process::Result result = ffmpeg::Run(std::move(request));
    if (!result.Started()) {
        error = ffmpeg::StartError(result, "export process pipes");
        removeTemporaryFiles();
        return false;
    }
    if (result.cancelled || !result.Succeeded()) {
        removeTemporaryFiles();
        if (result.cancelled) {
            error = "export cancelled";
        } else {
            // A process killed by a signal writes nothing: the tail then
            // ends on FFmpeg's own banner and names no cause. Say how it
            // ended, first, so that is never mistaken for a message.
            std::string ending;
            if (result.Signalled()) {
                ending = "FFmpeg was killed by signal " +
                         std::to_string(result.TerminationSignal()) + " (" +
                         strsignal(result.TerminationSignal()) + ")";
            } else {
                ending = "FFmpeg exited with status " +
                         std::to_string(result.ExitStatus());
            }
            error = result.error_tail.empty()
                        ? ending
                        : ending + ": " + result.error_tail;
        }
        return false;
    }

    // PA2 -- the film is rendered, its sound still lossless: bring it to
    // its loudness before the file takes its name.
    if (plan.loudness_outcome && !ApplyLoudnessTarget(plan, cancel, error)) {
        removeTemporaryFiles();
        return false;
    }

    std::error_code renameError;
    if (plan.settings.overwrite) {
        // POSIX rename is atomic and replaces a regular destination on macOS.
        std::filesystem::rename(plan.temporary_output_path,
                                plan.settings.output_path, renameError);
    } else {
        // link(2) atomically refuses an existing destination. Both paths are
        // siblings, so the hard-link commit cannot cross filesystems.
        if (link(plan.temporary_output_path.c_str(),
                 plan.settings.output_path.c_str()) != 0) {
            // exFAT has no hard links at all, and an external delivery drive
            // is routinely formatted that way -- the same constraint that
            // shaped ProjectStorage's exFAT support. Without this fallback a
            // fully rendered export is destroyed by removeTemporaryFiles at
            // the very last step, which is the worst possible moment to fail.
            // Checking then renaming cannot be atomic, so it stays a fallback
            // and never the first choice: the race it opens is narrower than
            // the cost of discarding a finished render.
            const int linkErrno = errno;
            if (linkErrno == ENOTSUP || linkErrno == EPERM ||
                linkErrno == EXDEV) {
                std::error_code existsError;
                if (std::filesystem::exists(plan.settings.output_path,
                                            existsError) ||
                    existsError) {
                    renameError = std::make_error_code(std::errc::file_exists);
                } else {
                    std::filesystem::rename(plan.temporary_output_path,
                                            plan.settings.output_path,
                                            renameError);
                }
            } else {
                renameError =
                    std::error_code(linkErrno, std::generic_category());
            }
        } else {
            std::error_code ignored;
            std::filesystem::remove(plan.temporary_output_path, ignored);
        }
    }
    if (renameError) {
        removeTemporaryFiles();
        error = "unable to commit exported file: " + renameError.message();
        return false;
    }
    removeTemporaryFiles();
    current.rendered_frames = plan.total_frames;
    current.fraction = 1.0;
    if (callback) callback(current);
    return true;
}

double Exporter::TaskFraction(const ExportTaskStep& step, double fraction) {
    const double files =
        step.files == 0 ? 1.0 : static_cast<double>(step.files);
    return std::clamp(
        (static_cast<double>(step.file) + std::clamp(fraction, 0.0, 1.0)) /
            files,
        0.0, 1.0);
}

std::string Exporter::ProgressDetail(const ExportTaskStep& step,
                                     const std::string& path,
                                     const ExportProgress& progress) {
    std::string detail;
    if (step.files > 1) {
        detail = "file " + std::to_string(step.file + 1) + "/" +
                 std::to_string(step.files) + " (" + std::to_string(step.file) +
                 " complete): ";
    }
    // PRD40 -- a segmented export says its pass, counted against the film.
    if (!progress.pass.empty()) {
        return detail + progress.pass + ", frames " +
               std::to_string(progress.pass_frames) + "/" +
               std::to_string(progress.pass_total) + " of " + path;
    }
    return detail + "frames " + std::to_string(progress.rendered_frames) + "/" +
           std::to_string(progress.total_frames) + " of " + path;
}

bool Exporter::RunPlan(const ExportPlan& plan, const MediaTaskRunner& runner,
                       const ExportTaskStep& step, std::string& error) {
    if (!runner) {
        return Run(plan, {}, nullptr, error);
    }
    const std::string path = plan.settings.output_path;
    bool committed = false;
    const MediaTaskSnapshot finished = runner(
        MediaTaskKind::Export, "Export " + path,
        [&plan, &step, &committed, path](MediaTaskContext& context,
                                         std::string& failure) {
            ExportProgress start;
            start.total_frames = plan.total_frames;
            context.SetProgress(TaskFraction(step, 0.0),
                                ProgressDetail(step, path, start));
            committed = Run(
                plan,
                [&](const ExportProgress& progress) {
                    context.SetProgress(TaskFraction(step, progress.fraction),
                                        ProgressDetail(step, path, progress));
                },
                context.CancelFlag(), failure);
            return committed;
        });
    // A file committed as the cancellation came is complete: it is there.
    if (finished.state == MediaTaskState::Succeeded ||
        (finished.state == MediaTaskState::Cancelled && committed)) {
        return true;
    }
    std::error_code ignored;
    std::filesystem::remove(plan.temporary_output_path, ignored);
    if (finished.state == MediaTaskState::Cancelled) {
        error = "export cancelled";
    } else {
        error = finished.error.empty() ? "export failed" : finished.error;
    }
    return false;
}
