// Out of lot -- the author, 29 October 2026: "D on a selected shot disables it:
// greyed, it is no longer seen or heard". The rule of ClipEnabled.h, read by
// the consumers that assemble the timeline: the monitor's Timeline (and
// verify_export through it), the export's plan, the timeline's audio plan
// (transcribe_timeline, analyze_timeline_audio), the sheets, timeline_stats
// and the cartons Record shows; and the toggle the window's D applies. The
// operation's round trip is in edit_tests, its journal format in
// operation_format_tests, MCP in mcp_tools_tests. Portable: no AppKit, and
// FFmpeg is never run.

#include "ClipEnabled.h"
#include "EditLog.h"
#include "Export.h"
#include "Operations.h"
#include "Subtitles.h"
#include "Timeline.h"
#include "TimelineSheets.h"
#include "TimelineStats.h"
#include "TimelineTranscription.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

int failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

template <typename Function>
void Test(const std::string& name, Function function) {
    const int before = failures;
    try {
        function();
        if (failures == before) std::cout << "PASS: " << name << '\n';
    } catch (const std::exception& exception) {
        ++failures;
        std::cerr << "FAIL: " << name << ": " << exception.what() << '\n';
    }
}

const Ulid kCamera = "01KDXB00000000000000000001";
const Ulid kOther = "01KDXB00000000000000000002";
const Ulid kBaseTrack = "01KDXB00000000000000000003";
const Ulid kOverTrack = "01KDXB00000000000000000004";
const Ulid kSoundTrack = "01KDXB00000000000000000005";
const Ulid kCaptionTrack = "01KDXB00000000000000000006";
const Ulid kWide = "01KDXB00000000000000000011";
const Ulid kTail = "01KDXB00000000000000000012";
const Ulid kOver = "01KDXB00000000000000000013";
const Ulid kSound = "01KDXB00000000000000000014";
const Ulid kCarton = "01KDXB00000000000000000015";
const Ulid kStyle = "01KDXB00000000000000000016";

// The export plan checks that each rush it reads exists: two empty files
// stand for them, never decoded.
std::filesystem::path Rushes() {
    static const std::filesystem::path directory = [] {
        const std::filesystem::path made =
            std::filesystem::temp_directory_path() / "cutmachine-clip-enabled";
        std::filesystem::create_directories(made);
        for (const char* name : {"camera.mp4", "other.mp4"})
            std::ofstream(made / name).put('\0');
        return made;
    }();
    return directory;
}

std::string Rush(const char* name) { return (Rushes() / name).string(); }

LibraryMedia Media(const Ulid& id, const std::string& path) {
    LibraryMedia media;
    media.id = id;
    media.path = path;
    media.filename = std::filesystem::path(path).filename().string();
    media.codec = "h264";
    media.width = 1920;
    media.height = 1080;
    media.orientation = "landscape";
    media.rate = {25, 1};
    media.duration = {1000, 25};
    media.has_video = true;
    media.has_audio = true;
    media.audio_rate = 48000;
    media.audio_channels = 2;
    media.metadata_complete = true;
    return media;
}

DocumentClip Clip(const Ulid& id, const Ulid& source, int64_t sourceIn,
                  int64_t duration, int64_t timelineIn) {
    DocumentClip clip;
    clip.id = id;
    clip.source_id = source;
    clip.source_in = {sourceIn, 25};
    clip.duration = {duration, 25};
    clip.timeline_in = {timelineIn, 25};
    return clip;
}

// The wide shot then a tail shot of another rush on V1, a cutaway over the
// first half second on V2, the wide shot's sound on A1, and one carton,
// left out of the documents the export plans: burning one needs the caption
// renderer, which only the application installs.
Document Fixture(bool withCarton = true) {
    Document document;
    document.sequence.width = 1920;
    document.sequence.height = 1080;
    document.sequence.frame_rate = {25, 1};
    document.library = {Media(kCamera, Rush("camera.mp4")),
                        Media(kOther, Rush("other.mp4"))};
    document.sources = {{kCamera, Rush("camera.mp4"), {25, 1}, {1000, 25}},
                        {kOther, Rush("other.mp4"), {25, 1}, {1000, 25}}};
    DocumentTrack base{kBaseTrack, "video", 0, {}};
    base.clips = {Clip(kWide, kCamera, 0, 50, 0),
                  Clip(kTail, kOther, 200, 25, 50)};
    DocumentTrack over{
        kOverTrack, "video", 1, {Clip(kOver, kCamera, 400, 12, 0)}};
    DocumentTrack sound{
        kSoundTrack, "audio", 2, {Clip(kSound, kCamera, 0, 50, 0)}};
    CaptionStyle style;
    style.id = kStyle;
    document.sequence.caption_styles = {style};
    DocumentClip carton;
    carton.id = kCarton;
    carton.timeline_in = {0, 25};
    carton.duration = {25, 25};
    carton.caption_group_id = kStyle;
    carton.caption_text = "Bonjour";
    DocumentTrack captions{kCaptionTrack, "caption", 3, {carton}};
    document.sequence.tracks = {base, over, sound};
    if (withCarton) document.sequence.tracks.push_back(captions);
    std::string error;
    if (!document.Validate(error))
        throw std::runtime_error("the fixture is invalid: " + error);
    return document;
}

void Disable(Document& document, EditLog& log, const std::vector<Ulid>& ids) {
    SetClipEnabledOperation operation;
    for (const Ulid& id : ids) operation.clips.push_back({id, false});
    EditError error = EditError::None;
    std::string message;
    if (!log.Apply(document, operation, error, message))
        throw std::runtime_error("set_clip_enabled: " + message);
}

Ulid ShownAt(const Document& document, const Ulid& trackId, int64_t frame) {
    const auto resolved =
        Timeline(document).ResolveTrack(trackId, RationalTime{frame, 25});
    return resolved ? resolved->clip_id : Ulid{};
}

// The export's inputs: every "-i" argument's value, in order.
std::vector<std::string> ExportInputs(const Document& document) {
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path();
    ExportSettings settings =
        Exporter::H264Preset((directory / "clip-enabled.mp4").string());
    settings.width = 320;
    settings.height = 180;
    settings.frame_rate = {25, 1};
    settings.ffmpeg_path = "ffmpeg";
    ExportPlan plan;
    std::string error;
    if (!Exporter::BuildPlan(document, (directory / "p.json").string(),
                             settings, plan, error))
        throw std::runtime_error("the export plan builds: " + error);
    std::vector<std::string> inputs;
    for (size_t index = 0; index + 1 < plan.arguments.size(); ++index)
        if (plan.arguments[index] == "-i")
            inputs.push_back(plan.arguments[index + 1]);
    return inputs;
}

size_t Count(const std::vector<std::string>& values, const std::string& value) {
    return static_cast<size_t>(std::count(values.begin(), values.end(), value));
}

}  // namespace

int main() {
    Test("a disabled clip is a hole in its track, its place kept", [] {
        EditLog log;
        Document document = Fixture();
        const RationalTime duration = Timeline(document).Duration();
        Check(ShownAt(document, kOverTrack, 5) == kOver,
              "the cutaway shows over the wide shot");
        Disable(document, log, {kOver, kTail});
        Check(ShownAt(document, kOverTrack, 5).empty(),
              "disabled, the cutaway leaves a hole in its track");
        const std::vector<ResolvedLayer> under =
            Timeline(document).ResolveTrackLayers(kBaseTrack, {5, 25});
        Check(under.size() == 1 && under.front().frame.clip_id == kWide,
              "and the wide shot under it shows through");
        Check(ShownAt(document, kBaseTrack, 60).empty(),
              "a disabled last shot is black where it lies");
        Check(Timeline(document).Duration() == duration,
              "and the timeline keeps its length, as Resolve's does");
        const std::vector<ResolvedFrame> upcoming =
            Timeline(document).ResolveUpcoming({45, 25}, 1, {2, 1});
        Check(std::none_of(upcoming.begin(), upcoming.end(),
                           [](const ResolvedFrame& frame) {
                               return frame.clip_id == kTail;
                           }),
              "playback does not warm up a disabled shot");
        EditError error = EditError::None;
        std::string message;
        Check(log.Undo(document, error, message) &&
                  ShownAt(document, kOverTrack, 5) == kOver &&
                  ShownAt(document, kBaseTrack, 60) == kTail,
              "enabled again, both are back: " + message);
    });

    Test("a dissolve drops its disabled side, as for a hidden angle", [] {
        EditLog log;
        Document document = Fixture();
        AddTransitionOperation dissolve;
        dissolve.transition.track_id = kBaseTrack;
        dissolve.transition.left_clip_id = kWide;
        dissolve.transition.right_clip_id = kTail;
        dissolve.transition.duration = {10, 25};
        EditError error = EditError::None;
        std::string message;
        Check(log.Apply(document, dissolve, error, message),
              "the dissolve applies: " + message);
        Check(Timeline(document)
                      .ResolveTrackLayers(kBaseTrack, {48, 25})
                      .size() == 2,
              "a dissolve shows both shots");
        Disable(document, log, {kTail});
        const std::vector<ResolvedLayer> layers =
            Timeline(document).ResolveTrackLayers(kBaseTrack, {48, 25});
        Check(layers.size() == 1 && layers.front().frame.clip_id == kWide,
              "the disabled incoming shot drops out of it");
    });

    Test("the export leaves a disabled clip out, picture and sound", [] {
        EditLog log;
        Document document = Fixture(false);
        const std::vector<std::string> before = ExportInputs(document);
        Check(Count(before, Rush("other.mp4")) == 1,
              "the tail shot is an input of the export");
        Disable(document, log, {kTail, kSound});
        const std::vector<std::string> after = ExportInputs(document);
        Check(Count(after, Rush("other.mp4")) == 0,
              "disabled, the tail shot is no input");
        Check(after.size() + 2 == before.size(),
              "nor is the disabled sound: " + std::to_string(before.size()) +
                  " inputs, then " + std::to_string(after.size()));
    });

    Test("a timeline whose every clip is disabled exports black and silence",
         [] {
             EditLog log;
             Document document = Fixture(false);
             Disable(document, log, {kWide, kTail, kOver, kSound});
             Check(ExportInputs(document).empty(),
                   "no media is read for a film with nothing in it");
         });

    Test("the timeline's mix leaves a disabled sound out", [] {
        EditLog log;
        Document document = Fixture();
        TimelineAudioPlan plan;
        std::string error;
        Check(BuildTimelineAudioPlan(document, "/rushes/p.json", plan, error) &&
                  plan.audio_clips == 1,
              "the wide shot's sound is mixed: " + error);
        const std::string heard = plan.cache_identity;
        Disable(document, log, {kSound});
        Check(
            !BuildTimelineAudioPlan(document, "/rushes/p.json", plan, error) &&
                error.find("no audible audio clips") != std::string::npos,
            "disabled, the timeline has nothing to transcribe: " + error);
        EditError editError = EditError::None;
        std::string message;
        Check(log.Undo(document, editError, message) &&
                  BuildTimelineAudioPlan(document, "/rushes/p.json", plan,
                                         error) &&
                  plan.cache_identity == heard,
              "enabled again, the mix is the one transcribed before");
    });

    Test("the sheets and timeline_stats do not count a disabled shot", [] {
        EditLog log;
        Document document = Fixture();
        TimelineStats stats;
        std::string error;
        Check(CalculateTimelineStats(document, stats, error),
              "stats: " + error);
        const int64_t shots = stats.visible_shots;
        TimelineSheetPlan plan;
        Check(BuildTimelineSheetPlan(document, TimelineSheetKind::Contact, {},
                                     plan, error),
              "sheet: " + error);
        Check(std::any_of(plan.frames.begin(), plan.frames.end(),
                          [](const TimelineSheetFrame& frame) {
                              return frame.clip_id == kOver;
                          }),
              "the cutaway is on the contact sheet");
        Disable(document, log, {kOver});
        Check(CalculateTimelineStats(document, stats, error) &&
                  stats.visible_shots < shots,
              "disabled, the cutaway is no visible shot");
        Check(BuildTimelineSheetPlan(document, TimelineSheetKind::Contact, {},
                                     plan, error) &&
                  std::none_of(plan.frames.begin(), plan.frames.end(),
                               [](const TimelineSheetFrame& frame) {
                                   return frame.clip_id == kOver;
                               }),
              "nor is it on the contact sheet");
        Check(OffScreenClipIds(document.sequence) == std::set<Ulid>{kOver},
              "the off-screen set names it");
    });

    Test("a disabled carton is not shown", [] {
        EditLog log;
        Document document = Fixture();
        Check(ActiveSubtitles(document, {10, 25}).size() == 1,
              "the carton shows on Record");
        Disable(document, log, {kCarton});
        Check(ActiveSubtitles(document, {10, 25}).empty(),
              "disabled, it does not");
    });

    Test("D toggles a selection as Resolve does", [] {
        Document document = Fixture();
        SetClipEnabledOperation toggle =
            ToggleClipsEnabled(document, {kWide, kSound, kWide, "missing"});
        Check(toggle.clips.size() == 2 && !toggle.clips[0].enabled &&
                  !toggle.clips[1].enabled,
              "an enabled selection is disabled, each clip once");
        document.FindClip(kWide)->enabled = false;
        toggle = ToggleClipsEnabled(document, {kWide, kSound});
        Check(toggle.clips.size() == 2 && !toggle.clips[0].enabled &&
                  !toggle.clips[1].enabled,
              "a clip still enabled disables the whole selection");
        document.FindClip(kSound)->enabled = false;
        toggle = ToggleClipsEnabled(document, {kWide, kSound});
        Check(toggle.clips.size() == 2 && toggle.clips[0].enabled &&
                  toggle.clips[1].enabled,
              "a selection all disabled is enabled");
        document.FindTrack(kSoundTrack)->locked = true;
        toggle = ToggleClipsEnabled(document, {kSound});
        Check(toggle.clips.empty(),
              "a clip on a locked track is left as it plays");
        Check(ToggleClipsEnabled(document, {}).clips.empty(),
              "no selection, nothing to do");
    });

    return failures == 0 ? 0 : 1;
}
