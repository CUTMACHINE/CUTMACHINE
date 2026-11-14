#pragma once

// REMOTION-PARITY-2026-09 REM2 -- docs/roadmap/REMOTION-PARITY-2026-09.md,
// docs/MOTION_FORMAT.md. The sound of a composition: every video that is not
// muted and every audio node, each at the time its sequences, repeats and
// instances give it, mixed at 48 kHz stereo by the engine itself.
//
// The mix is a pure function of the composition, its props and the media,
// computed sample by sample in exact time: a node's sound starts on the
// first sample at or after its first instant, its volume -- a linear gain,
// animatable as any number is -- is evaluated at each sample's exact time,
// and the samples are summed without limiting. What a surface does with it
// (a track in a rendered video, a layer of a timeline's mix, the monitor's
// sound) is theirs; the samples are the same everywhere, which is what the
// corpus scene C13 checks before any codec touches them.
//
// A sound plays at its own speed or not at all: a video at another
// playbackRate, or under a sequence whose timeScale changes its pace,
// plays no sound unless muted -- refused as UnsupportedAudioRate, not
// resampled into a pitch nobody chose.
//
// Portable: the media arrive through the measurer (MotionEvaluate.h),
// decoded by MotionMedia.h in the engine and made up in the tests.

#include "MotionEvaluate.h"

#include <cstdint>
#include <map>
#include <string>

// How many samples a composition's sound lasts: its duration at 48 kHz,
// the last partial sample included.
int64_t MotionAudioSampleCount(const MotionComposition& composition);

// Samples [first, first + count) of the composition's sound, sample 0 at
// its frame 0. `mix` is resized to `count`; silence where nothing plays.
// Refusals: those of the evaluator for a media (MediaMissing,
// MediaHasNoAudio, UnknownComposition, CompositionCycle, UnknownProp...),
// UnsupportedAudioRate, TimeOverflow.
bool MixMotionAudio(const MotionComposition& composition,
                    const mcp_json::Value& props,
                    const MotionEvaluationContext& context,
                    MotionMeasurer& measurer, int64_t first, int64_t count,
                    MotionPcm& mix, MotionError& error);

// The sounds a composition plays, without decoding them: one entry per
// voice, in tree order -- node path and uid, media, the window of
// composition samples it can be heard in and the media sample its first
// one reads -- as the JSON `{"voices":[...]}`. `audible` counts them.
// What a surface asks before deciding to give a render a sound track.
bool DescribeMotionAudio(const MotionComposition& composition,
                         const mcp_json::Value& props,
                         const MotionEvaluationContext& context,
                         MotionMeasurer& measurer, std::string& json,
                         size_t& audible, MotionError& error);

// A mix as a WAV file: 32-bit float, 48 kHz stereo, so that what an encoder
// reads is the mix to the bit. Refuses to overwrite.
bool WriteMotionAudioWav(const MotionPcm& pcm, const std::string& path,
                         std::string& error);

// REM2 -- a measurer that reads media and nothing else: what the sound of a
// composition needs, where no font is at hand (the export plans a
// timeline's mix in the model). `src` resolves against `directory`,
// `media` through `paths` (MotionMediaPaths); text and images are refused.
class MotionMediaMeasurer final : public MotionMeasurer {
public:
    MotionMediaMeasurer(std::string directory,
                        std::map<std::string, std::string> paths)
        : directory_(std::move(directory)), paths_(std::move(paths)) {}

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

private:
    bool Path(const std::string& src, const std::string& media,
              std::string& path, MotionError& error) const;

    std::string directory_;
    std::map<std::string, std::string> paths_;
};
