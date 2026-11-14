#include "CaptionRender.h"
#include "Cli.h"
#include "ClipAnimation.h"
#include "ClipFraming.h"
#include "ClipSpeed.h"
#include "ColorEffects.h"
#include "ColorManagement.h"
#include "Document.h"
#include "EditLog.h"
#include "Export.h"
#include "ExportVerification.h"
#include "Ingest.h"
#include "Json.h"
#include "LookLut.h"
#include "LookLutStorage.h"
#include "McpLiveBackend.h"
#include "McpTools.h"
#include "Multicam.h"
#include "ProjectStorage.h"
#include "Timeline.h"
#include "TimelineAudioReport.h"
#include "TimelineCheck.h"
#include "TransitionRender.h"
#include "Ulid.h"

extern "C" {
#include <libavformat/avformat.h>
}

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <set>
#include <sstream>
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

std::string Quote(const std::string& value) {
    std::string result = "'";
    for (char character : value) {
        if (character == '\'')
            result += "'\\''";
        else
            result += character;
    }
    return result + "'";
}

size_t Count(const std::string& text, const std::string& needle) {
    size_t count = 0;
    size_t offset = 0;
    while ((offset = text.find(needle, offset)) != std::string::npos) {
        ++count;
        offset += needle.size();
    }
    return count;
}

bool MeanFirstRgbFrame(const std::string& path, int width, int height,
                       std::array<double, 3>& mean) {
    const auto raw = std::filesystem::temp_directory_path() /
                     (GenerateUlid() + "-export-rgb.raw");
    const std::string decode =
        Quote(FFMPEG_EXECUTABLE) + " -hide_banner -loglevel error -xerror -i " +
        Quote(path) + " -map 0:v:0 -frames:v 1 -pix_fmt rgb24 -f rawvideo -y " +
        Quote(raw.string());
    if (std::system(decode.c_str()) != 0) {
        std::filesystem::remove(raw);
        return false;
    }
    std::ifstream input(raw, std::ios::binary);
    const std::vector<unsigned char> pixels{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
    std::filesystem::remove(raw);
    if (pixels.size() != static_cast<size_t>(width) * height * 3) {
        return false;
    }
    mean = {0.0, 0.0, 0.0};
    for (size_t offset = 0; offset < pixels.size(); offset += 3) {
        for (size_t channel = 0; channel < 3; ++channel) {
            mean[channel] += pixels[offset + channel];
        }
    }
    for (double& channel : mean) {
        channel /= static_cast<double>(width) * height;
    }
    return true;
}

// ITW1 -- ROADMAP.md. A numbered source: every frame carries its own index
// as twelve large black-or-white blocks, which survive lossy encoding, so an
// exported frame can be traced back to the exact source frame it shows.
constexpr int kCodeWidth = 64;
constexpr int kCodeHeight = 48;
constexpr int kCodeBlock = 16;
constexpr int kCodeBits = 12;

bool WriteNumberedSource(const std::filesystem::path& path, MediaRate rate,
                         int frames, bool still = false) {
    const auto raw = path.string() + ".gray";
    {
        std::ofstream output(raw, std::ios::binary | std::ios::trunc);
        std::vector<char> pixels(kCodeWidth * kCodeHeight);
        for (int frame = 0; frame < frames; ++frame) {
            // Index zero is reserved for "no picture" (the black base).
            const int code = still ? 1 : frame + 1;
            for (int y = 0; y < kCodeHeight; ++y) {
                for (int x = 0; x < kCodeWidth; ++x) {
                    const int bit = (x / kCodeBlock) + (4 * (y / kCodeBlock));
                    pixels[(y * kCodeWidth) + x] =
                        static_cast<char>(((code >> bit) & 1) != 0 ? 235 : 16);
                }
            }
            output.write(pixels.data(),
                         static_cast<std::streamsize>(pixels.size()));
        }
        if (!output) return false;
    }
    const std::string encode =
        Quote(FFMPEG_EXECUTABLE) +
        " -hide_banner -loglevel error -f rawvideo -pix_fmt gray -s " +
        std::to_string(kCodeWidth) + 'x' + std::to_string(kCodeHeight) +
        " -framerate " + std::to_string(rate.num) + '/' +
        std::to_string(rate.den) + " -i " + Quote(raw) +
        " -c:v libx264 -crf 0 -g 12 -pix_fmt yuv420p -y " +
        Quote(path.string());
    const bool encoded = std::system(encode.c_str()) == 0;
    std::filesystem::remove(raw);
    return encoded;
}

// Returns the source index shown by every frame of `path`, or -1 where the
// frame carries no picture.
bool ReadFrameCodes(const std::string& path, std::vector<int>& codes) {
    const auto raw = std::filesystem::temp_directory_path() /
                     (GenerateUlid() + "-export-codes.gray");
    const std::string decode =
        Quote(FFMPEG_EXECUTABLE) + " -hide_banner -loglevel error -i " +
        Quote(path) + " -map 0:v:0 -pix_fmt gray -f rawvideo -y " +
        Quote(raw.string());
    if (std::system(decode.c_str()) != 0) {
        std::filesystem::remove(raw);
        return false;
    }
    std::ifstream input(raw, std::ios::binary);
    const std::vector<unsigned char> pixels{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
    std::filesystem::remove(raw);
    const size_t frameSize = static_cast<size_t>(kCodeWidth) * kCodeHeight;
    if (pixels.empty() || pixels.size() % frameSize != 0) return false;
    codes.clear();
    for (size_t offset = 0; offset < pixels.size(); offset += frameSize) {
        int code = 0;
        for (int bit = 0; bit < kCodeBits; ++bit) {
            const int centerX = ((bit % 4) * kCodeBlock) + (kCodeBlock / 2);
            const int centerY = ((bit / 4) * kCodeBlock) + (kCodeBlock / 2);
            const unsigned char luma =
                pixels[offset + (static_cast<size_t>(centerY) * kCodeWidth) +
                       centerX];
            if (luma > 126) code |= 1 << bit;
        }
        codes.push_back(code - 1);
    }
    return true;
}

// Reproduces how FFmpeg evaluated the placement this exporter used to write,
// `setpts=PTS-STARTPTS+(value/rate)/TB`: a double quotient, truncated.
bool OldPlacementTruncates(int64_t frame, MediaRate rate) {
    const double seconds = static_cast<double>(frame * rate.den) / rate.num;
    const double timeBase = static_cast<double>(rate.den) / rate.num;
    return static_cast<int64_t>(seconds / timeBase) != frame;
}

// ... and its enable window `lt(t, in + duration)`, which could keep a shot
// on screen one frame past its end.
bool OldWindowOverflows(int64_t frame, int64_t duration, MediaRate rate) {
    const double timeBase = static_cast<double>(rate.den) / rate.num;
    const double end = (static_cast<double>(frame * rate.den) / rate.num) +
                       (static_cast<double>(duration * rate.den) / rate.num);
    return static_cast<double>(frame + duration) * timeBase < end;
}

int64_t FirstTruncatingFrame(int64_t from, MediaRate rate) {
    for (int64_t frame = from; frame < from + 400; ++frame) {
        if (OldPlacementTruncates(frame, rate)) return frame;
    }
    return from;
}

// `managed` runs the same timeline through the colour-managed graph, whose
// placement differs (F1.3: a clip that starts after the first frame is led
// by one transparent frame), so the frame exactness has to hold there too.
void CheckFrameExactExport(const std::filesystem::path& directory,
                           MediaRate rate, const std::string& label,
                           bool managed = false) {
    const auto at = [&](int64_t frames) {
        return RationalTime{frames * rate.den, rate.num};
    };
    constexpr int kSourceFrames = 1300;
    const auto media = directory / ("numbered-" + label + ".mp4");
    Check(WriteNumberedSource(media, rate, kSourceFrames),
          label + ": numbered source is generated");

    // Cuts where the former float placement truncated at this cadence.
    const int64_t cutA = FirstTruncatingFrame(200, rate);
    const int64_t cutB = FirstTruncatingFrame(400, rate);
    const int64_t cutC = FirstTruncatingFrame(900, rate);
    const int64_t end = cutC + 171;
    int64_t cutawayIn = cutC + 1;
    int64_t cutawayDuration = 122;
    for (int64_t frame = cutC + 1; frame < cutC + 30; ++frame) {
        for (int64_t duration = 100; duration < 140; ++duration) {
            if (frame + duration < end &&
                OldWindowOverflows(frame, duration, rate)) {
                cutawayIn = frame;
                cutawayDuration = duration;
                frame = cutC + 30;
                break;
            }
        }
    }

    Document document;
    document.sequence.width = kCodeWidth;
    document.sequence.height = kCodeHeight;
    document.sequence.frame_rate = rate;
    document.color_management.enabled = managed;
    LibraryMedia library;
    library.id = "01K4F7W1000000000000000001";
    library.path = media.string();
    library.filename = media.filename().string();
    library.codec = "h264";
    library.width = kCodeWidth;
    library.height = kCodeHeight;
    library.rate = rate;
    library.duration = at(kSourceFrames);
    library.has_video = true;
    library.has_audio = false;
    library.metadata_complete = true;
    document.library = {library};
    document.sources = {{library.id, media.string(), rate, at(kSourceFrames)}};
    struct Shot {
        int64_t timeline_in;
        int64_t duration;
        int64_t source_in;
    };
    const std::vector<Shot> primary = {
        {0, cutA, 1},
        {cutA, cutB - cutA, 400},
        {cutB, cutC - cutB, 30},
        {cutC, end - cutC, 700},
    };
    const Shot cutaway{cutawayIn, cutawayDuration, 1100};
    DocumentTrack lower{"01K4F7W1000000000000000002", "video", 0, {}};
    int clipNumber = 0;
    for (const Shot& shot : primary) {
        DocumentClip clip;
        clip.id = "01K4F7W10000000000000001" + std::to_string(10 + clipNumber);
        ++clipNumber;
        clip.source_id = library.id;
        clip.source_in = at(shot.source_in);
        clip.duration = at(shot.duration);
        clip.timeline_in = at(shot.timeline_in);
        clip.include_audio = false;
        lower.clips.push_back(clip);
    }
    DocumentTrack upper{"01K4F7W1000000000000000003", "video", 1, {}};
    DocumentClip cutawayClip;
    cutawayClip.id = "01K4F7W1000000000000000004";
    cutawayClip.source_id = library.id;
    cutawayClip.source_in = at(cutaway.source_in);
    cutawayClip.duration = at(cutaway.duration);
    cutawayClip.timeline_in = at(cutaway.timeline_in);
    cutawayClip.include_audio = false;
    upper.clips.push_back(cutawayClip);
    document.sequence.tracks = {lower, upper};

    const auto projectPath = directory / ("numbered-" + label + ".json");
    const auto output = directory / ("numbered-" + label + "-export.mp4");
    ExportSettings settings = Exporter::H264Preset(output.string());
    settings.encoder = ExportEncoder::H264Software;
    settings.width = kCodeWidth;
    settings.height = kCodeHeight;
    settings.frame_rate = rate;
    settings.ffmpeg_path = FFMPEG_EXECUTABLE;
    ExportPlan plan;
    std::string error;
    const bool rendered = Exporter::BuildPlan(document, projectPath.string(),
                                              settings, plan, error) &&
                          Exporter::Run(plan, {}, nullptr, error);
    Check(rendered, label + ": numbered timeline exports: " + error);
    std::vector<int> codes;
    Check(rendered && ReadFrameCodes(output.string(), codes),
          label + ": exported frames decode");
    Check(codes.size() == static_cast<size_t>(end),
          label + ": export has one frame per timeline frame, got " +
              std::to_string(codes.size()));
    int mismatches = 0;
    std::string firstMismatches;
    for (int64_t frame = 0;
         frame < end && frame < static_cast<int64_t>(codes.size()); ++frame) {
        int64_t expected = -1;
        const auto covers = [&](const Shot& shot) {
            return frame >= shot.timeline_in &&
                   frame < shot.timeline_in + shot.duration;
        };
        if (covers(cutaway)) {
            expected = cutaway.source_in + (frame - cutaway.timeline_in);
        } else {
            for (const Shot& shot : primary) {
                if (covers(shot))
                    expected = shot.source_in + (frame - shot.timeline_in);
            }
        }
        if (codes[frame] != expected) {
            if (mismatches < 6) {
                firstMismatches += " frame " + std::to_string(frame) +
                                   " shows " + std::to_string(codes[frame]) +
                                   " instead of " + std::to_string(expected) +
                                   ';';
            }
            ++mismatches;
        }
    }
    Check(mismatches == 0,
          label + ": every exported frame is the expected source frame (" +
              std::to_string(mismatches) + " wrong:" + firstMismatches +
              " cuts " + std::to_string(cutA) + ' ' + std::to_string(cutB) +
              ' ' + std::to_string(cutC) + ", cutaway " +
              std::to_string(cutaway.timeline_in) + '+' +
              std::to_string(cutaway.duration) + ')');

    // ITW1 criterion 4 -- the engine names diverging frames itself, so the
    // agent does not have to write this comparison again.
    ExportVerificationSettings verification;
    verification.ffmpeg_path = FFMPEG_EXECUTABLE;
    ExportVerificationReport report;
    Check(VerifyExport(document, directory, output.string(), verification,
                       report, error) &&
              report.expected_frames == end && report.exported_frames == end &&
              report.divergent_frames == 0 && report.checked_frames == end &&
              report.ambiguous_frames == 0,
          label + ": verification finds the exact export exact: " + error +
              ' ' + SerializeExportVerification(report));

    // The same file against a document whose second shot starts one source
    // frame later, and whose cutaway ends one frame earlier: exactly what the
    // former float placement produced, seen from the other side.
    Document moved = document;
    moved.sequence.tracks[0].clips[1].source_in = at(primary[1].source_in + 1);
    moved.sequence.tracks[1].clips[0].duration = at(cutaway.duration - 1);
    Check(VerifyExport(moved, directory, output.string(), verification, report,
                       error),
          label + ": verification runs on a diverging document: " + error);
    const std::string serialized = SerializeExportVerification(report);
    bool shotOffset = false;
    bool cutawayTail = false;
    for (const ExportDivergence& divergence : report.divergences) {
        if (divergence.kind == "offset" && divergence.offset == -1 &&
            divergence.first_frame == cutA &&
            divergence.last_frame == cutB - 1 &&
            divergence.clip_id == moved.sequence.tracks[0].clips[1].id &&
            divergence.expected_source_frame == primary[1].source_in + 1) {
            shotOffset = true;
        }
        if (divergence.kind == "wrong_picture" &&
            divergence.first_frame ==
                cutaway.timeline_in + cutaway.duration - 1 &&
            divergence.last_frame == divergence.first_frame) {
            cutawayTail = true;
        }
    }
    Check(shotOffset && cutawayTail &&
              report.divergent_frames == (cutB - cutA) + 1 &&
              serialized.find("\"exact\":false") != std::string::npos,
          label +
              ": verification names the shifted shot and the extra "
              "cutaway frame, and nothing else: " +
              serialized);

    // One frame alone expecting the next source frame: a one-frame clip cut
    // out of the second shot. The closer look that dismisses thumbnail
    // noise must still report it, on that frame and that frame only.
    Document single = document;
    {
        std::vector<DocumentClip>& clips = single.sequence.tracks[0].clips;
        const DocumentClip shot = clips[1];
        const int64_t middle = cutA + (cutB - cutA) / 2;
        DocumentClip head = shot;
        head.duration = at(middle - cutA);
        DocumentClip one = shot;
        one.id = "01K4F7W1000000000000000091";
        one.timeline_in = at(middle);
        one.source_in = at(primary[1].source_in + (middle - cutA) + 1);
        one.duration = at(1);
        DocumentClip tail = shot;
        tail.id = "01K4F7W1000000000000000092";
        tail.timeline_in = at(middle + 1);
        tail.source_in = at(primary[1].source_in + (middle + 1 - cutA));
        tail.duration = at(cutB - middle - 1);
        clips[1] = head;
        clips.insert(clips.begin() + 2, {one, tail});
        Check(VerifyExport(single, directory, output.string(), verification,
                           report, error) &&
                  report.divergences.size() == 1 &&
                  report.divergences[0].kind == "offset" &&
                  report.divergences[0].offset == -1 &&
                  report.divergences[0].first_frame == middle &&
                  report.divergences[0].last_frame == middle &&
                  report.unconfirmed_offset_frames == 0,
              label + ": a one-frame offset is confirmed and reported: " +
                  error + ' ' + SerializeExportVerification(report));
    }
}

// F1.5 -- ROADMAP.md. Two cameras over the same stretch, the wide one on V1
// under the close-up on V2. The exported pictures, read back frame by frame,
// must be the active angle's -- through the angle above it when the lower one
// is active -- and follow a switch; verify_export must expect the same.
void CheckMulticamExport(const std::filesystem::path& directory) {
    const MediaRate rate{25, 1};
    const auto at = [&](int64_t frames) { return RationalTime{frames, 25}; };
    const auto media = directory / "multicam-numbered.mp4";
    Check(WriteNumberedSource(media, rate, 300),
          "multicam: numbered source is generated");
    Document document;
    document.sequence.width = kCodeWidth;
    document.sequence.height = kCodeHeight;
    document.sequence.frame_rate = rate;
    LibraryMedia library;
    library.id = "01K9MX00000000000000000001";
    library.path = media.string();
    library.filename = media.filename().string();
    library.codec = "h264";
    library.width = kCodeWidth;
    library.height = kCodeHeight;
    library.orientation = "landscape";
    library.rate = rate;
    library.duration = at(300);
    library.has_video = true;
    library.has_audio = false;
    document.library = {library};
    document.sources = {{library.id, media.string(), rate, at(300)}};
    const auto clip = [&](const Ulid& id, int64_t sourceIn, int64_t duration,
                          int64_t timelineIn) {
        DocumentClip made;
        made.id = id;
        made.source_id = library.id;
        made.source_in = at(sourceIn);
        made.duration = at(duration);
        made.timeline_in = at(timelineIn);
        made.include_audio = false;
        return made;
    };
    const Ulid wide = "01K9MX00000000000000000011";
    const Ulid tail = "01K9MX00000000000000000012";
    const Ulid close = "01K9MX00000000000000000013";
    DocumentTrack lower{"01K9MX00000000000000000002", "video", 0, {}};
    lower.clips = {clip(wide, 10, 60, 0), clip(tail, 200, 20, 60)};
    DocumentTrack upper{
        "01K9MX00000000000000000003", "video", 1, {clip(close, 100, 60, 0)}};
    document.sequence.tracks = {lower, upper};

    EditLog log;
    EditError editError = EditError::None;
    std::string error;
    AddMulticamGroupOperation group;
    Check(BuildAddMulticamGroup(document, "Interview",
                                {{wide, "Large"}, {close, "Serré"}}, wide,
                                group, editError, error) &&
              log.Apply(document, group, editError, error),
          "multicam: the group applies with the wide angle active: " + error);

    const auto exportCodes = [&](const std::string& name,
                                 std::vector<int>& codes) {
        const auto output = directory / name;
        ExportSettings settings = Exporter::H264Preset(output.string());
        settings.encoder = ExportEncoder::H264Software;
        settings.width = kCodeWidth;
        settings.height = kCodeHeight;
        settings.frame_rate = rate;
        settings.ffmpeg_path = FFMPEG_EXECUTABLE;
        ExportPlan plan;
        const bool rendered =
            Exporter::BuildPlan(document,
                                (directory / "multicam.json").string(),
                                settings, plan, error) &&
            Exporter::Run(plan, {}, nullptr, error);
        Check(rendered, "multicam: " + name + " exports: " + error);
        Check(rendered && ReadFrameCodes(output.string(), codes) &&
                  codes.size() == 80,
              "multicam: " + name + " has one picture per timeline frame");
        return output;
    };
    const auto mismatches = [](const std::vector<int>& codes,
                               int64_t angleSource) {
        int wrong = 0;
        for (int64_t frame = 0; frame < static_cast<int64_t>(codes.size());
             ++frame) {
            const int64_t expected =
                frame < 60 ? angleSource + frame : 200 + (frame - 60);
            if (codes[frame] != expected) {
                ++wrong;
            }
        }
        return wrong;
    };
    ExportVerificationSettings verification;
    verification.ffmpeg_path = FFMPEG_EXECUTABLE;
    ExportVerificationReport report;

    std::vector<int> wideCodes;
    const auto wideExport = exportCodes("multicam-wide.mp4", wideCodes);
    Check(wideCodes.size() == 80 && mismatches(wideCodes, 10) == 0,
          "multicam: every frame shows the wide angle through the hidden "
          "close-up above it");
    Check(VerifyExport(document, directory, wideExport.string(), verification,
                       report, error) &&
              report.divergent_frames == 0 && report.checked_frames == 80,
          "multicam: verify_export expects the wide angle too: " + error + ' ' +
              SerializeExportVerification(report));

    SetMulticamActiveAngleOperation set;
    Check(BuildSetMulticamActiveAngle(document, {}, close, set, editError,
                                      error) &&
              log.Apply(document, set, editError, error),
          "multicam: the close-up becomes the active angle: " + error);
    std::vector<int> closeCodes;
    const auto closeExport = exportCodes("multicam-close.mp4", closeCodes);
    Check(closeCodes.size() == 80 && mismatches(closeCodes, 100) == 0,
          "multicam: after the switch every frame shows the close-up");
    Check(VerifyExport(document, directory, closeExport.string(), verification,
                       report, error) &&
              report.divergent_frames == 0,
          "multicam: verify_export follows the switch: " + error);
    Check(VerifyExport(document, directory, wideExport.string(), verification,
                       report, error) &&
              report.divergent_frames == 60,
          "multicam: the wide export no longer matches the switched timeline "
          "on exactly the grouped stretch: " +
              SerializeExportVerification(report));
}

Document Fixture(const std::string& mediaPath) {
    Document document;
    const Ulid sourceId = "01K40000000000000000000001";
    LibraryMedia media;
    media.id = sourceId;
    media.path = mediaPath;
    media.filename = "source.mp4";
    media.codec = "h264";
    media.width = 320;
    media.height = 180;
    media.pixel_format = "yuv420p";
    media.color_range = "tv";
    media.color_space = "bt709";
    media.color_transfer = "bt709";
    media.color_primaries = "bt709";
    media.rate = {25, 1};
    media.duration = {25, 25};
    media.orientation = "landscape";
    media.has_audio = true;
    media.audio_rate = 48000;
    media.audio_channels = 2;
    document.library = {media};
    document.sources = {{sourceId, mediaPath, {25, 1}, {25, 25}}};
    document.sequence.tracks = {
        {"01K40000000000000000000002",
         "video",
         0,
         {{"01K40000000000000000000003",
           sourceId,
           {0, 25},
           {10, 25},
           {5, 25},
           false}}},
        {"01K40000000000000000000004",
         "video",
         1,
         {{"01K40000000000000000000005",
           sourceId,
           {10, 25},
           {5, 25},
           {10, 25},
           false}}},
        {"01K40000000000000000000006",
         "audio",
         2,
         {{"01K40000000000000000000007",
           sourceId,
           {0, 25},
           {10, 25},
           {5, 25},
           true}}},
    };
    return document;
}

// FI2 -- ROADMAP.md. Offline media is required only when the selected
// output consumes it, including dissolve handles and the limiter's memory.
void CheckOfflineExportInputs(const std::filesystem::path& directory,
                              const std::string& mediaPath) {
    Document base = Fixture(mediaPath);
    LibraryMedia missing = base.library.front();
    missing.id = GenerateUlid();
    missing.path = (directory / "selection-offline.mp4").string();
    base.library.push_back(missing);
    base.sources.push_back(
        {missing.id, missing.path, missing.rate, missing.duration});
    DocumentTrack video = base.sequence.tracks.front();
    video.clips.front().timeline_in = {0, 25};
    DocumentClip offline = video.clips.front();
    offline.id = GenerateUlid();
    offline.source_id = missing.id;
    offline.timeline_in = {15, 25};
    video.clips.push_back(offline);
    base.sequence.tracks = {video};
    ExportSettings settings =
        Exporter::H264Preset((directory / "selection-output.mp4").string());
    settings.frame_rate = {25, 1};
    settings.width = 320;
    settings.height = 180;
    settings.ffmpeg_path = FFMPEG_EXECUTABLE;
    const auto check = [&](const Document& document,
                           const ExportSettings& requested, bool expected,
                           const std::string& label) {
        std::string error;
        Check(document.Validate(error), label + " validates: " + error);
        ExportPlan plan;
        const bool ok = Exporter::BuildPlan(
            document, (directory / "selection.json").string(), requested, plan,
            error);
        Check(ok == expected &&
                  (expected || error.find(missing.path) != std::string::npos),
              label + ": " + error);
    };
    settings.window_end = 10;
    check(base, settings, true, "offline clip outside the output window");
    settings.window_first = 15;
    settings.window_end = 25;
    check(base, settings, false, "offline clip inside the output window");
    settings.window_first = 0;
    settings.window_end = 0;

    Document hidden = base;
    hidden.sequence.tracks.front().clips.pop_back();
    DocumentTrack second = video;
    second.id = GenerateUlid();
    second.index = 1;
    second.visible = false;
    second.clips = {offline};
    hidden.sequence.tracks.push_back(second);
    check(hidden, settings, true, "offline media on a hidden video track");
    second.kind = "audio";
    second.muted = true;
    second.clips.front().include_audio = true;
    hidden.sequence.tracks.back() = second;
    check(hidden, settings, true, "offline media on a muted audio track");
    hidden.sequence.tracks.back().muted = false;
    DocumentTrack solo = hidden.sequence.tracks.front();
    solo.id = GenerateUlid();
    solo.index = 2;
    solo.kind = "audio";
    solo.solo = true;
    solo.clips.front().id = GenerateUlid();
    solo.clips.front().include_audio = true;
    hidden.sequence.tracks.push_back(solo);
    check(hidden, settings, true, "offline audio excluded by another solo");

    Document multicam = base;
    multicam.sequence.tracks.front().clips.pop_back();
    second.kind = "video";
    second.visible = true;
    second.muted = false;
    second.clips.front().include_audio = false;
    second.clips.front().timeline_in = {0, 25};
    multicam.sequence.tracks.push_back(second);
    MulticamGroup group;
    group.angles = {{GenerateUlid(), "online",
                     multicam.sequence.tracks.front().clips.front().id},
                    {GenerateUlid(), "offline", second.clips.front().id}};
    group.active_angle_id = group.angles.front().id;
    multicam.sequence.multicam_groups = {group};
    check(multicam, settings, true, "offline inactive multicam angle");

    Document dissolve = base;
    auto& clips = dissolve.sequence.tracks.front().clips;
    clips.back().timeline_in = {10, 25};
    clips.back().source_in = {5, 25};
    dissolve.sequence.transitions = {{GenerateUlid(),
                                      video.id,
                                      clips.front().id,
                                      clips.back().id,
                                      "cross_dissolve",
                                      {4, 25},
                                      TransitionAlignment::Center}};
    settings.window_first = 8;
    settings.window_end = 10;
    check(dissolve, settings, false,
          "offline incoming dissolve handle reaches the window");
    dissolve.sequence.transitions.clear();
    check(dissolve, settings, true,
          "without a dissolve the offline shot starts after the window");

    Document sound = base;
    sound.sequence.tracks.front().clips = {video.clips.front()};
    sound.sequence.tracks.front().clips.front().timeline_in = {50, 25};
    second.kind = "audio";
    second.clips.front().include_audio = true;
    second.clips.front().duration = {5, 25};
    sound.sequence.tracks.push_back(second);
    settings.window_first = 50;
    settings.window_end = 60;
    check(sound, settings, true, "offline sound before the limiter lead");
    sound.sequence.tracks.back().clips.front().timeline_in = {30, 25};
    check(sound, settings, false, "offline sound inside the limiter lead");
}

// F1.3 -- ROADMAP.md, linear composite. A flat colour, encoded losslessly,
// so that every pixel of every frame carries the same known signal.
bool WriteFlatSource(const std::filesystem::path& path, int width, int height,
                     const std::string& color) {
    const std::string encode =
        Quote(FFMPEG_EXECUTABLE) +
        " -hide_banner -loglevel error -f lavfi -i 'color=c=" + color +
        ":s=" + std::to_string(width) + 'x' + std::to_string(height) +
        ":r=25:d=3' -c:v libx264 -crf 0 -pix_fmt yuv420p -y " +
        Quote(path.string());
    return std::system(encode.c_str()) == 0;
}

// Decoded frames as full-range float RGB, read the way the managed export
// reads a source before its working-space cube. FFmpeg's planar order is
// green, blue, red.
struct FloatFrames {
    int width = 0;
    int height = 0;
    std::vector<float> samples;

    size_t Count() const {
        const size_t frame = static_cast<size_t>(width) * height * 3;
        return frame == 0 ? 0 : samples.size() / frame;
    }

    RgbColor At(size_t frame, int x, int y) const {
        const size_t plane = static_cast<size_t>(width) * height;
        const size_t base =
            (frame * plane * 3) + (static_cast<size_t>(y) * width) + x;
        return {samples[base + (2 * plane)], samples[base],
                samples[base + plane]};
    }
};

bool ReadFloatFrames(const std::string& path, int width, int height,
                     FloatFrames& frames) {
    const auto raw = std::filesystem::temp_directory_path() /
                     (GenerateUlid() + "-export-float.raw");
    const std::string decode =
        Quote(FFMPEG_EXECUTABLE) + " -hide_banner -loglevel error -i " +
        Quote(path) +
        " -map 0:v:0 -vf scale=out_range=full,format=gbrpf32le -f rawvideo "
        "-pix_fmt gbrpf32le -y " +
        Quote(raw.string());
    if (std::system(decode.c_str()) != 0) {
        std::filesystem::remove(raw);
        return false;
    }
    std::ifstream input(raw, std::ios::binary);
    const std::string bytes{std::istreambuf_iterator<char>(input),
                            std::istreambuf_iterator<char>()};
    std::filesystem::remove(raw);
    const size_t frameBytes =
        static_cast<size_t>(width) * height * 3 * sizeof(float);
    if (bytes.empty() || bytes.size() % frameBytes != 0) return false;
    frames.width = width;
    frames.height = height;
    frames.samples.resize(bytes.size() / sizeof(float));
    std::memcpy(frames.samples.data(), bytes.data(), bytes.size());
    return true;
}

double MaxChannelDistance(const RgbColor& left, const RgbColor& right) {
    return std::max({std::abs(left.red - right.red),
                     std::abs(left.green - right.green),
                     std::abs(left.blue - right.blue)});
}

std::string Describe(const RgbColor& color) {
    return '(' + std::to_string(color.red) + ' ' + std::to_string(color.green) +
           ' ' + std::to_string(color.blue) + ')';
}

RgbColor Mix(const RgbColor& from, const RgbColor& to, double amount) {
    return {from.red + ((to.red - from.red) * amount),
            from.green + ((to.green - from.green) * amount),
            from.blue + ((to.blue - from.blue) * amount)};
}

// F1.3 -- ROADMAP.md, linear composite (18 September 2026). Under colour
// management a dissolve, a translucent clip and a pillarboxed translucent
// clip mix scene-linear light and meet the display transform once, as on
// the monitor. Every expected pixel comes from the OpenColorIO path the
// export samples into its cubes, evaluated here in full precision.
//
// Tolerance: what separates the file from that reference is sampling, not
// the transform -- the 65^3 working cube read trilinearly on the source
// signal, the 1D ACEScct shaper, the 65^3 display cube, 10-bit 4:2:0 output
// and x265 at CRF 18. 0.012 of full scale is three 8-bit codes; the same
// pixels mixed as display signals, as the ITW5 graph did, miss the linear
// mix by more than four times that, which is checked first.
void CheckLinearManagedComposite(const std::filesystem::path& directory) {
    constexpr int kWidth = 96;
    constexpr int kHeight = 54;
    constexpr double kTolerance = 0.012;
    struct Flat {
        const char* name;
        const char* color;
        int width;
    };
    // The last source is 4:3: it is pillarboxed, 12 pixels on each side.
    const std::array<Flat, 4> flats = {{
        {"dark", "0x283040", kWidth},
        {"bright", "0xD8C8A0", kWidth},
        {"green", "0x40A060", kWidth},
        {"narrow", "0x8040C0", 72},
    }};
    Document document;
    document.sequence.width = kWidth;
    document.sequence.height = kHeight;
    document.sequence.frame_rate = {25, 1};
    document.color_management.enabled = true;
    std::array<RgbColor, 4> signals{};
    for (size_t index = 0; index < flats.size(); ++index) {
        const auto path =
            directory / ("linear-" + std::string(flats[index].name) + ".mp4");
        FloatFrames frames;
        if (!WriteFlatSource(path, flats[index].width, kHeight,
                             flats[index].color) ||
            !ReadFloatFrames(path.string(), flats[index].width, kHeight,
                             frames) ||
            frames.Count() == 0) {
            Check(false, "linear composite: flat sources are generated");
            return;
        }
        signals[index] = frames.At(0, flats[index].width / 2, kHeight / 2);
        document.sources.push_back(
            {GenerateUlid(), path.string(), {25, 1}, {75, 25}});
    }
    const auto clip = [&](size_t source, int64_t in, int64_t frames,
                          std::vector<ClipEffect> effects,
                          EffectParamValue opacity) {
        DocumentClip result;
        result.source_id = document.sources[source].id;
        result.source_in = {5, 25};
        result.duration = {frames, 25};
        result.timeline_in = {in, 25};
        result.include_audio = false;
        result.effects = std::move(effects);
        result.opacity = opacity;
        return result;
    };
    const std::vector<ClipEffect> darker = {
        {GenerateUlid(), "color.exposure", {{"amount", {-1, 1}}}}};
    const std::vector<ClipEffect> brighter = {
        {GenerateUlid(), "color.exposure", {{"amount", {1, 1}}}}};
    const std::vector<ClipEffect> richer = {
        {GenerateUlid(), "color.saturation", {{"amount", {1, 2}}}}};
    const DocumentTrack lower{
        GenerateUlid(),
        "video",
        0,
        {clip(0, 0, 20, darker, {1, 1}), clip(1, 20, 40, brighter, {1, 1})}};
    const DocumentTrack upper{
        GenerateUlid(),
        "video",
        1,
        {clip(2, 30, 10, {}, {1, 2}), clip(3, 45, 10, richer, {1, 2})}};
    document.sequence.tracks = {lower, upper};
    // Centred on the cut at 20: the second shot fades in over frames 15 to
    // 24, at k/10 on frame 15 + k, as the monitor's transition progress.
    document.sequence.transitions = {
        {GenerateUlid(), lower.id, lower.clips[0].id, lower.clips[1].id,
         "cross_dissolve", RationalTime{10, 25}, TransitionAlignment::Center}};

    const ColorManagementSettings& color = document.color_management;
    const auto working = [&](size_t source,
                             const std::vector<ClipEffect>& effects) {
        RgbColor result;
        std::string failure;
        Check(OpenColorIoWorkingColor(color, ResolveColorGrade(effects, {}),
                                      signals[source], result, failure),
              "linear composite: reference working colour: " + failure);
        return result;
    };
    const auto display = [&](const RgbColor& linear) {
        RgbColor result;
        std::string failure;
        Check(OpenColorIoDisplayColor(color, linear, result, failure),
              "linear composite: reference display colour: " + failure);
        return result;
    };
    const RgbColor dark = working(0, darker);
    const RgbColor bright = working(1, brighter);
    const RgbColor green = working(2, {});
    const RgbColor narrow = working(3, richer);
    // The sheets and read_frame keep the combined OpenColorIO cube, source
    // straight to display, and no composite: an ungraded clip must read the
    // same through the export's two halves, at every sampled signal.
    {
        std::string combined;
        std::string failure;
        Check(BuildOpenColorIoCube(color, combined, failure),
              "linear composite: combined cube builds: " + failure);
        std::istringstream lines(combined);
        std::string line;
        std::vector<RgbColor> entries;
        while (std::getline(lines, line)) {
            if (line.empty() ||
                std::isalpha(static_cast<unsigned char>(line.front())) != 0)
                continue;
            std::istringstream values(line);
            RgbColor entry;
            values >> entry.red >> entry.green >> entry.blue;
            entries.push_back(entry);
        }
        const int edge = kOpenColorIoLutEdge;
        const bool complete =
            entries.size() == static_cast<size_t>(edge) * edge * edge;
        double worstSheet = 0.0;
        for (int blue = 0; complete && blue < edge; blue += 8) {
            for (int green = 0; green < edge; green += 8) {
                for (int red = 0; red < edge; red += 8) {
                    RgbColor linear;
                    OpenColorIoWorkingColor(
                        color, {},
                        {red / (edge - 1.0), green / (edge - 1.0),
                         blue / (edge - 1.0)},
                        linear, failure);
                    const size_t index = static_cast<size_t>(
                        (((blue * edge) + green) * edge) + red);
                    worstSheet = std::max(
                        worstSheet,
                        MaxChannelDistance(display(linear), entries[index]));
                }
            }
        }
        Check(complete && worstSheet < 1e-3,
              "linear composite: the sheets' combined transform is the "
              "export's working then display path, worst " +
                  std::to_string(worstSheet));
        std::cout << "linear composite: sheets against export path, worst "
                  << worstSheet << '\n';
    }
    // The export's shaper reads negative linear values as zero, where the
    // monitor still separates them (ColorManagement.cc): the display
    // transform must not tell them apart either, in SDR and in HLG.
    for (const bool hlg : {false, true}) {
        ColorManagementSettings output = color;
        if (hlg) {
            output.output_gamut = "rec2020";
            output.output_transfer = "hlg";
        }
        double spread = 0.0;
        RgbColor zero;
        std::string failure;
        OpenColorIoDisplayColor(output, {0.0, 0.0, 0.0}, zero, failure);
        for (const double negative : {-0.0069, -0.004, -0.001}) {
            RgbColor shown;
            OpenColorIoDisplayColor(output, {negative, negative, negative},
                                    shown, failure);
            spread = std::max(spread, MaxChannelDistance(shown, zero));
        }
        Check(spread < 1e-3, std::string("linear composite: negative light "
                                         "displays as zero light in ") +
                                 (hlg ? "HLG" : "SDR") + ", spread " +
                                 std::to_string(spread));
    }
    const auto separates = [&](const RgbColor& below, const RgbColor& above) {
        return MaxChannelDistance(display(Mix(below, above, 0.5)),
                                  Mix(display(below), display(above), 0.5)) >
               4 * kTolerance;
    };
    Check(separates(dark, bright) && separates(bright, green) &&
              separates(bright, narrow),
          "linear composite: the fixture tells linear light from "
          "display-signal mixing");

    const auto output = directory / "linear-composite.mp4";
    ExportSettings settings =
        Exporter::HevcMain10SoftwarePreset(output.string());
    settings.width = kWidth;
    settings.height = kHeight;
    settings.frame_rate = {25, 1};
    settings.ffmpeg_path = FFMPEG_EXECUTABLE;
    ExportPlan plan;
    std::string error;
    const bool rendered =
        Exporter::BuildPlan(document, (directory / "linear.json").string(),
                            settings, plan, error) &&
        Exporter::Run(plan, {}, nullptr, error);
    Check(rendered, "linear composite: the managed timeline exports: " + error);
    FloatFrames shown;
    const bool decoded =
        rendered && ReadFloatFrames(output.string(), kWidth, kHeight, shown) &&
        shown.Count() == 60;
    Check(decoded, "linear composite: the export decodes to 60 frames");
    if (!decoded) return;

    double worst = 0.0;
    const auto expect = [&](size_t frame, int x, const RgbColor& expected,
                            const std::string& what) {
        const RgbColor actual = shown.At(frame, x, kHeight / 2);
        const double distance = MaxChannelDistance(actual, expected);
        worst = std::max(worst, distance);
        Check(distance <= kTolerance,
              "linear composite: " + what + " (frame " + std::to_string(frame) +
                  ", x " + std::to_string(x) + "): got " + Describe(actual) +
                  ", expected " + Describe(expected));
    };
    constexpr int kCenter = kWidth / 2;
    expect(10, kCenter, display(dark), "first shot alone");
    expect(14, kCenter, display(dark), "frame before the dissolve");
    for (int step = 0; step < 10; ++step) {
        expect(15 + step, kCenter, display(Mix(dark, bright, step / 10.0)),
               "dissolve step " + std::to_string(step) + "/10");
    }
    expect(25, kCenter, display(bright), "frame after the dissolve");
    expect(35, kCenter, display(Mix(bright, green, 0.5)),
           "half-opaque clip over the second shot");
    // Six pixels from the pillarbox edge on each side, clear of the 4:2:0
    // chroma filter: inside, the clip mixes; outside, the shot below shows
    // untouched.
    expect(50, kCenter, display(Mix(bright, narrow, 0.5)),
           "half-opaque pillarboxed clip");
    expect(50, 18, display(Mix(bright, narrow, 0.5)),
           "inside the pillarbox edge");
    expect(50, 6, display(bright), "outside the pillarbox edge");
    expect(50, kWidth - 7, display(bright), "outside the right pillarbox");
    expect(57, kCenter, display(bright), "after the translucent clips");
    std::cout << "linear composite: worst channel distance " << worst
              << " (tolerance " << kTolerance << ")\n";
}

// F1.3 -- ROADMAP.md, wheels and curves (18 September 2026). One flat clip
// graded by a wheel stage and a curve stage, exported with and without
// colour management. The expected pixel is the portable calculation the
// monitor's shader is checked against (tests/color_grade_shader_tests.mm):
// ApplyResolvedColorGrade on the source signal without colour management,
// and OpenColorIoWorkingColor then OpenColorIoDisplayColor, which evaluate
// it in ACEScct in full precision, with it. The tolerance is the linear
// composite's, for the same sampling reasons.
//
// The source is BT.709 YCbCr and says so. WriteFlatSource's untagged
// fixture is encoded with BT.601, FFmpeg's default, while the unmanaged
// export labels its output BT.709: an ungraded clip of it already comes
// back 0.0115 away from the signal read here (measured), the fixture's
// disagreement rather than the grade's.
void CheckWheelsAndCurvesExport(const std::filesystem::path& directory) {
    constexpr int kWidth = 96;
    constexpr int kHeight = 54;
    constexpr double kTolerance = 0.012;
    const auto source = directory / "wheels-curves-source.mp4";
    const std::string encode =
        Quote(FFMPEG_EXECUTABLE) +
        " -hide_banner -loglevel error -f lavfi -i "
        "'color=c=0x5A7896:s=" +
        std::to_string(kWidth) + 'x' + std::to_string(kHeight) +
        ":r=25:d=3' -vf scale=out_color_matrix=bt709:out_range=tv -c:v "
        "libx264 -crf 0 -pix_fmt yuv420p -colorspace bt709 -color_primaries "
        "bt709 -color_trc bt709 -color_range tv -y " +
        Quote(source.string());
    FloatFrames frames;
    if (std::system(encode.c_str()) != 0 ||
        !ReadFloatFrames(source.string(), kWidth, kHeight, frames) ||
        frames.Count() == 0) {
        Check(false, "wheels and curves: the flat source is generated");
        return;
    }
    const RgbColor signal = frames.At(0, kWidth / 2, kHeight / 2);
    const ClipEffect wheels{GenerateUlid(),
                            "color.wheels",
                            {{"lift_r", {1, 20}},
                             {"gamma_master", {1, 4}},
                             {"gain_b", {-1, 5}},
                             {"gain_master", {1, 10}}}};
    const ClipEffect curves{GenerateUlid(),
                            "color.curves",
                            {{"master_x0", {0, 1}},
                             {"master_y0", {0, 1}},
                             {"master_x1", {1, 4}},
                             {"master_y1", {3, 20}},
                             {"master_x2", {1, 2}},
                             {"master_y2", {1, 2}},
                             {"master_x3", {3, 4}},
                             {"master_y3", {17, 20}},
                             {"master_x4", {1, 1}},
                             {"master_y4", {1, 1}},
                             {"g_x0", {0, 1}},
                             {"g_y0", {1, 20}},
                             {"g_x1", {1, 1}},
                             {"g_y1", {19, 20}}}};

    // Identical grades share one temporary cube; wheel stages that differ
    // only by their knobs must not.
    {
        Document shared;
        shared.sequence.width = kWidth;
        shared.sequence.height = kHeight;
        shared.sequence.frame_rate = {25, 1};
        shared.sources.push_back(
            {GenerateUlid(), source.string(), {25, 1}, {75, 25}});
        ClipEffect other = wheels;
        other.params["lift_r"] = {1, 10};
        std::vector<DocumentClip> clips;
        for (const ClipEffect& effect : {wheels, other, wheels}) {
            DocumentClip clip;
            clip.source_id = shared.sources[0].id;
            clip.source_in = {0, 25};
            clip.duration = {5, 25};
            clip.timeline_in = {static_cast<int64_t>(clips.size()) * 5, 25};
            clip.include_audio = false;
            clip.effects = {effect};
            clips.push_back(clip);
        }
        shared.sequence.tracks = {{GenerateUlid(), "video", 0, clips}};
        ExportSettings settings = Exporter::HevcMain10SoftwarePreset(
            (directory / "wheels-shared.mp4").string());
        settings.width = kWidth;
        settings.height = kHeight;
        settings.frame_rate = {25, 1};
        ExportPlan plan;
        std::string error;
        Check(Exporter::BuildPlan(shared,
                                  (directory / "wheels-shared.json").string(),
                                  settings, plan, error) &&
                  plan.clip_grade_luts.size() == 2,
              "wheels and curves: two different wheel stages get two cubes, "
              "the same one shares its cube: " +
                  std::to_string(plan.clip_grade_luts.size()) + " " + error);
    }

    for (const bool managed : {false, true}) {
        const std::string mode = managed ? "managed" : "unmanaged";
        Document document;
        document.sequence.width = kWidth;
        document.sequence.height = kHeight;
        document.sequence.frame_rate = {25, 1};
        document.color_management.enabled = managed;
        document.sources.push_back(
            {GenerateUlid(), source.string(), {25, 1}, {75, 25}});
        DocumentClip clip;
        clip.source_id = document.sources[0].id;
        clip.source_in = {5, 25};
        clip.duration = {20, 25};
        clip.timeline_in = {0, 25};
        clip.include_audio = false;
        clip.effects = {wheels, curves};
        document.sequence.tracks = {
            {GenerateUlid(), "video", 0, {clip}},
        };

        const auto expected = [&](const std::vector<ClipEffect>& effects) {
            const ResolvedColorGrade grade = ResolveColorGrade(effects, {});
            if (!managed) {
                const auto graded =
                    ApplyResolvedColorGrade({static_cast<float>(signal.red),
                                             static_cast<float>(signal.green),
                                             static_cast<float>(signal.blue)},
                                            grade);
                return RgbColor{std::clamp(graded[0], 0.0f, 1.0f),
                                std::clamp(graded[1], 0.0f, 1.0f),
                                std::clamp(graded[2], 0.0f, 1.0f)};
            }
            RgbColor working;
            RgbColor shown;
            std::string failure;
            Check(OpenColorIoWorkingColor(document.color_management, grade,
                                          signal, working, failure) &&
                      OpenColorIoDisplayColor(document.color_management,
                                              working, shown, failure),
                  "wheels and curves: reference colour: " + failure);
            return shown;
        };
        const RgbColor graded = expected({wheels, curves});
        // Each stage must matter on its own, or the pixel below could
        // pass with one of them dropped.
        Check(MaxChannelDistance(graded, expected({})) > 4 * kTolerance &&
                  MaxChannelDistance(graded, expected({wheels})) >
                      2 * kTolerance &&
                  MaxChannelDistance(graded, expected({curves})) >
                      2 * kTolerance &&
                  MaxChannelDistance(graded, expected({curves, wheels})) >
                      kTolerance,
              "wheels and curves (" + mode +
                  "): the fixture tells each stage and their order apart");

        const auto output = directory / ("wheels-curves-" + mode + ".mp4");
        ExportSettings settings =
            Exporter::HevcMain10SoftwarePreset(output.string());
        settings.width = kWidth;
        settings.height = kHeight;
        settings.frame_rate = {25, 1};
        settings.ffmpeg_path = FFMPEG_EXECUTABLE;
        ExportPlan plan;
        std::string error;
        const bool rendered =
            Exporter::BuildPlan(
                document, (directory / ("wheels-" + mode + ".json")).string(),
                settings, plan, error) &&
            Exporter::Run(plan, {}, nullptr, error);
        std::string exported = "wheels and curves (" + mode + "): exports: ";
        exported += error;
        Check(rendered, exported);
        FloatFrames shown;
        if (!rendered ||
            !ReadFloatFrames(output.string(), kWidth, kHeight, shown) ||
            shown.Count() != 20) {
            Check(false, "wheels and curves (" + mode + "): decodes 20 frames");
            continue;
        }
        const RgbColor actual = shown.At(10, kWidth / 2, kHeight / 2);
        const double distance = MaxChannelDistance(actual, graded);
        Check(distance <= kTolerance, "wheels and curves (" + mode + "): got " +
                                          Describe(actual) + ", expected " +
                                          Describe(graded) + " from source " +
                                          Describe(signal));
        std::cout << "wheels and curves, " << mode << ": distance " << distance
                  << " (tolerance " << kTolerance << ")\n";
    }
}

// F1.3 -- the linear composite belongs to colour-managed projects only. An
// unmanaged timeline reaches FFmpeg exactly as before, argument for
// argument, filter graph included: same process, same file. Recorded from
// the exporter at 27a3f88, with its temporary paths named. DEC28 --
// docs/roadmap/REEL-2026-09.md: each clip's `overlay` now names the 10-bit
// 4:4:4 format its canvas has, the one change since, decided on 23 October
// 2026.
constexpr const char* kUnmanagedArguments = R"GOLDEN(-hide_banner
-nostdin
-y
-progress
pipe:1
-nostats
-ss
0.0
-t
0.48
-i
<source>
-ss
0.32
-t
0.28
-i
<source>
-ss
0.4
-t
0.2
-i
<source>
-ss
0.0
-t
0.4
-i
<source>
-f
image2
-framerate
25/1
-start_number
0
-i
<captions0>/%06d.png
-filter_complex
color=c=black:s=320x180:r=25/1:d=0.8,format=yuv444p10le,setparams=colorspace=bt709[base0];[0:v:0]fps=25/1,scale=320:180:force_original_aspect_ratio=decrease:flags=lanczos,setsar=1,setparams=colorspace=bt709,format=gbrp16le,lut3d=file='<grade0>':interp=trilinear,setpts=PTS-STARTPTS+5[v0];[base0][v0]overlay=format=yuv444p10:x=(W-w)/2:y=(H-h)/2:eof_action=repeat:repeatlast=1:shortest=0:enable=gte(t\,((9*1)/50))*lt(t\,((33*1)/50))[base1];[1:v:0]fps=25/1,scale=320:180:force_original_aspect_ratio=decrease:flags=lanczos,setsar=1,setparams=colorspace=bt709,format=yuva444p10le,fade=t=in:s=0:n=4:alpha=1,setpts=PTS-STARTPTS+13[v1];[base1][v1]overlay=format=yuv444p10:x=(W-w)/2:y=(H-h)/2:eof_action=repeat:repeatlast=1:shortest=0:enable=gte(t\,((25*1)/50))*lt(t\,((39*1)/50))[base2];[2:v:0]fps=25/1,scale=320:180:force_original_aspect_ratio=decrease:flags=lanczos,setsar=1,setparams=colorspace=bt709,format=yuva444p10le,colorchannelmixer=aa=(2/3),setpts=PTS-STARTPTS+10[v2];[base2][v2]overlay=format=yuv444p10:x=(W-w)/2:y=(H-h)/2:eof_action=repeat:repeatlast=1:shortest=0:enable=gte(t\,((19*1)/50))*lt(t\,((29*1)/50))[base3];[base3]format=yuv420p10le,null[captionbase];[4:v]setpts=PTS-STARTPTS+0,format=rgba,scale=out_color_matrix=bt709:out_range=tv,format=yuva444p10le[captions];[captionbase][captions]overlay=format=yuv420p10:eof_action=pass:shortest=0[captioned];[captioned]format=yuv420p10le,setparams=range=limited:color_primaries=bt709:color_trc=bt709:colorspace=bt709[video];[3:a:0]atrim=duration=0.4,asetpts=PTS-STARTPTS,aresample=48000,aformat=sample_fmts=fltp:channel_layouts=stereo,afade=t=in:st=0:d=0.003,afade=t=out:st=0.397:d=0.003,adelay=9600S:all=1[a0];[a0]amix=inputs=1:duration=longest:normalize=0,alimiter=limit=0.668344:level=0:latency=1,apad,atrim=duration=0.8[audio]
-map
[video]
-map
[audio]
-c:v
libx265
-profile:v
main10
-preset
medium
-crf
18
-pix_fmt
yuv420p10le
-tag:v
hvc1
-color_primaries
bt709
-color_trc
bt709
-colorspace
bt709
-color_range
tv
-c:a
aac
-b:a
320000
-ar
48000
-movflags
+faststart
-r
25/1
-frames:v
20
<output>
)GOLDEN";

void CheckUnmanagedArgumentsUnchanged(const std::filesystem::path& directory,
                                      const std::filesystem::path& media) {
    Document document = Fixture(media.string());
    document.sequence.tracks[0].clips[0].effects = {
        {"01K4000000000000000000000A", "color.contrast", {{"amount", {1, 4}}}}};
    document.sequence.tracks[1].clips[0].opacity = {2, 3};
    const DocumentClip incoming{"01K40000000000000000000008",
                                document.sources[0].id,
                                {10, 25},
                                {5, 25},
                                {15, 25},
                                false};
    document.sequence.tracks[0].clips.push_back(incoming);
    document.sequence.transitions = {{
        "01K40000000000000000000009",
        document.sequence.tracks[0].id,
        document.sequence.tracks[0].clips[0].id,
        incoming.id,
        "cross_dissolve",
        {4, 25},
        TransitionAlignment::Center,
    }};
    document.sequence.caption_styles = {
        {"01K4000000000000000000000B", "Sans", 18, "#ffffff", "bottom"}};
    DocumentClip caption;
    caption.id = "01K4000000000000000000000C";
    caption.source_in = {0, 25};
    caption.timeline_in = {2, 25};
    caption.duration = {8, 25};
    caption.include_audio = false;
    caption.caption_group_id = document.sequence.caption_styles[0].id;
    caption.caption_text = "Témoin";
    document.sequence.tracks.push_back(
        {"01K4000000000000000000000D", "caption", 3, {caption}});

    ExportSettings settings = Exporter::HevcMain10SoftwarePreset(
        (directory / "unmanaged.mp4").string());
    settings.width = 320;
    settings.height = 180;
    settings.frame_rate = {25, 1};
    ExportPlan plan;
    std::string error;
    Check(Exporter::BuildPlan(document, (directory / "unmanaged.json").string(),
                              settings, plan, error),
          "unmanaged plan builds: " + error);
    std::vector<std::pair<std::string, std::string>> names = {
        {media.string(), "<source>"}, {plan.temporary_output_path, "<output>"}};
    for (size_t index = 0; index < plan.clip_grade_luts.size(); ++index) {
        names.emplace_back(plan.clip_grade_luts[index].path,
                           "<grade" + std::to_string(index) + '>');
    }
    for (size_t index = 0; index < plan.caption_assets.size(); ++index) {
        names.emplace_back(plan.caption_assets[index].directory,
                           "<captions" + std::to_string(index) + '>');
    }
    std::string arguments;
    for (std::string argument : plan.arguments) {
        for (const auto& [path, name] : names) {
            for (size_t at = argument.find(path); at != std::string::npos;
                 at = argument.find(path, at + name.size())) {
                argument.replace(at, path.size(), name);
            }
        }
        arguments += argument + '\n';
    }
    Check(plan.temporary_lut_path.empty() && plan.display_shaper.path.empty() &&
              arguments == kUnmanagedArguments,
          "an unmanaged export runs exactly the FFmpeg command it ran before "
          "the linear composite; now:\n" +
              arguments);
}

// PRD16 -- ROADMAP.md. One vocabulary of export, one default. The keys are
// what --export --preset and the catalog's `preset` accept; the default is
// what every surface encodes when its caller names no codec and no preset.
void CheckPresetVocabulary() {
    std::set<std::string> keys;
    for (const ExportPresetDescriptor& descriptor : Exporter::Presets()) {
        const std::string key = descriptor.key;
        ExportPresetId found = ExportPresetId::HevcMaster;
        Check(!key.empty() &&
                  std::all_of(key.begin(), key.end(),
                              [](char c) {
                                  return (c >= 'a' && c <= 'z') ||
                                         (c >= '0' && c <= '9') || c == '_';
                              }) &&
                  keys.insert(key).second && Exporter::FindPreset(key, found) &&
                  found == descriptor.id,
              "preset key '" + key + "' is lowercase, unique and found back");
    }
    Check(Exporter::PresetKeys() ==
              "hevc_high_quality, hevc_master, hevc_web_1080, "
              "hevc_uhd_delivery, h264",
          "the preset keys, in the window's order: " + Exporter::PresetKeys());
    ExportPresetId preset = ExportPresetId::HevcMaster;
    Check(!Exporter::FindPreset("H264", preset) &&
              !Exporter::FindPreset("hevc", preset),
          "a preset is named by its exact key");

    // The H.264 preset is what export_timeline builds for codec h264.
    const ExportSettings h264 = Exporter::SettingsForPreset(
        ExportPresetId::H264, "out.mp4", 1080, 1920, {30000, 1001});
    ExportSettings expected = Exporter::H264Preset("out.mp4");
    expected.width = 1080;
    expected.height = 1920;
    expected.frame_rate = {30000, 1001};
    Check(h264.encoder == expected.encoder && !h264.main10 &&
              h264.video_bitrate == expected.video_bitrate &&
              h264.width == 1080 && h264.height == 1920 &&
              h264.frame_rate.num == 30000 && h264.frame_rate.den == 1001 &&
              h264.audio_bitrate == expected.audio_bitrate,
          "the h264 preset is H264Preset at the sequence's format");
    const ExportSettings odd = Exporter::SettingsForPreset(
        ExportPresetId::H264, "out.mp4", 1281, 721, {25, 1});
    Check(odd.width == 1282 && odd.height == 722,
          "the h264 preset rounds an odd side up, as every preset does: the "
          "default never refuses a timeline --export used to deliver");

    // One default: H.264 High for SDR, HEVC Main10 for an HLG delivery.
    Document sdr;
    sdr.sequence.width = 1280;
    sdr.sequence.height = 720;
    sdr.sequence.frame_rate = {25, 1};
    Check(Exporter::DefaultPreset(sdr) == ExportPresetId::H264,
          "an SDR timeline defaults to H.264");
    const ExportSettings sdrDefault =
        Exporter::DefaultSettings(sdr, "default.mp4");
    Check(Exporter::IsH264(sdrDefault.encoder) && !sdrDefault.main10 &&
              sdrDefault.width == 1280 && sdrDefault.height == 720 &&
              sdrDefault.output_path == "default.mp4",
          "the SDR default is H.264 High 8-bit at the timeline's format");
    Document hlg = sdr;
    hlg.color_management.enabled = true;
    hlg.color_management.output_gamut = "rec2020";
    hlg.color_management.output_transfer = "hlg";
    Check(Exporter::DefaultPreset(hlg) == ExportPresetId::HevcHighQuality,
          "an HLG delivery defaults to HEVC Main10");
    const ExportSettings hlgDefault =
        Exporter::DefaultSettings(hlg, "default.mp4");
    Check(hlgDefault.encoder == ExportEncoder::HevcVideoToolbox &&
              hlgDefault.main10 && hlgDefault.width == 1280 &&
              hlgDefault.height == 720,
          "the HLG default is HEVC Main10 at the timeline's format");
    Document unmanagedHlg = hlg;
    unmanagedHlg.color_management.enabled = false;
    Check(Exporter::DefaultPreset(unmanagedHlg) == ExportPresetId::H264,
          "an HLG output that colour management does not apply is SDR, as "
          "the export tags it");
    const auto& presets = Exporter::Presets();
    Check(presets[Exporter::PresetIndex(Exporter::DefaultPreset(sdr))].id ==
                  ExportPresetId::H264 &&
              presets[Exporter::PresetIndex(Exporter::DefaultPreset(hlg))].id ==
                  ExportPresetId::HevcHighQuality,
          "a menu built from Presets() finds the default by PresetIndex");

    // A preset beside the codec it encodes; refused beside another, and an
    // unknown key named with the ones there are.
    std::string error;
    Check(Exporter::ReadPreset("hevc_master", "hevc", "--preset", preset,
                               error) &&
              preset == ExportPresetId::HevcMaster &&
              Exporter::ReadPreset("h264", "", "--preset", preset, error) &&
              preset == ExportPresetId::H264,
          "a preset reads alone or with its own codec: " + error);
    Check(!Exporter::ReadPreset("h264", "hevc", "export_timeline.preset",
                                preset, error) &&
              error.find("export_timeline.preset") != std::string::npos &&
              error.find("contradicts codec 'hevc'") != std::string::npos,
          "an H.264 preset beside codec hevc is refused by name: " + error);
    Check(!Exporter::ReadPreset("hevc_web_1080", "h264", "--preset", preset,
                                error) &&
              error.find("contradicts codec 'h264'") != std::string::npos,
          "an HEVC preset beside codec h264 is refused by name: " + error);
    Check(!Exporter::ReadPreset("prores", "", "--preset", preset, error) &&
              error.find(Exporter::PresetKeys()) != std::string::npos,
          "an unknown preset is refused with every key: " + error);

    ExportSettings software = Exporter::H264Preset("a.mp4");
    Exporter::UseSoftwareEncoder(software);
    ExportSettings hevc = Exporter::HevcMain10Preset("a.mp4");
    Exporter::UseSoftwareEncoder(hevc);
    Check(software.encoder == ExportEncoder::H264Software &&
              hevc.encoder == ExportEncoder::HevcSoftware,
          "software keeps the codec and swaps in x264 or x265");
}

// PRD16 -- ROADMAP.md. What get_task says of an export in progress, and
// what is left when a background task's runner did not let the encoding
// through: no partial file, whatever ran in Run's place.
void CheckExportTaskSteps(const std::filesystem::path& directory) {
    ExportProgress progress;
    progress.rendered_frames = 120;
    progress.total_frames = 250;
    progress.fraction = 0.48;
    Check(Exporter::ProgressDetail({0, 1}, "/Films/a.mp4", progress) ==
              "frames 120/250 of /Films/a.mp4",
          "a single export says its frames out of the total");
    Check(Exporter::ProgressDetail({1, 3}, "/Films/b.mp4", progress) ==
              "file 2/3 (1 complete): frames 120/250 of /Films/b.mp4",
          "a batch says which file is encoded and how many are complete");
    // PRD40 -- a segmented export says its pass, against the film's frames:
    // it read "frames 2000/2978" of a film of 1489.
    ExportProgress segmented;
    segmented.rendered_frames = 2000;
    segmented.total_frames = 2978;
    segmented.pass = "assembly";
    segmented.pass_frames = 511;
    segmented.pass_total = 1489;
    Check(Exporter::ProgressDetail({0, 1}, "/Films/c.mp4", segmented) ==
              "assembly, frames 511/1489 of /Films/c.mp4",
          "a segmented export names its pass and counts the film's frames");
    Check(std::abs(Exporter::TaskFraction({1, 3}, 0.5) - 0.5) < 1e-9 &&
              Exporter::TaskFraction({0, 1}, 2.0) == 1.0 &&
              Exporter::TaskFraction({2, 3}, 0.0) > 0.66,
          "the task's share counts the files complete and the current one");

    ExportPlan plan;
    plan.settings.output_path = (directory / "prd16-task.mp4").string();
    plan.temporary_output_path =
        (directory / "prd16-task.cutmachine-0.part.mp4").string();
    for (const MediaTaskState state :
         {MediaTaskState::Failed, MediaTaskState::Cancelled}) {
        std::ofstream(plan.temporary_output_path) << "partial";
        std::string kindSeen;
        const MediaTaskRunner refusing =
            [&](MediaTaskKind kind, std::string label, MediaTaskManager::Work) {
                kindSeen = MediaTaskKindName(kind) + (" " + label);
                MediaTaskSnapshot finished;
                finished.state = state;
                finished.error = "stand-in refused";
                return finished;
            };
        std::string error;
        Check(!Exporter::RunPlan(plan, refusing, {}, error) &&
                  !std::filesystem::exists(plan.temporary_output_path) &&
                  kindSeen == "Export Export " + plan.settings.output_path &&
                  error == (state == MediaTaskState::Cancelled
                                ? "export cancelled"
                                : "stand-in refused"),
              "a file the task's runner did not encode leaves no partial file "
              "and says why: " +
                  error);
    }
}

}  // namespace

// R3 -- docs/roadmap/REEL-2026-09.md, criterion 1. Four shots, one per
// speed case the ticket names, exported and read back frame by frame
// against ClipSpeed.h's rule. The graph reopens the source at an effective
// cadence and lets `fps` pick; this is what says the two agree.
// R4 -- docs/roadmap/REEL-2026-09.md, decided on 8 October 2026. A freeze
// is a clip at speed 0/1: a 2 s hold at 25 i/s exports 50 frames, every one
// the source frame asked for, and the rush's very last frame can be held.
void CheckFreezeExport(const std::filesystem::path& directory) {
    constexpr MediaRate kRate{25, 1};
    constexpr int kSourceFrames = 100;
    const auto source = directory / "freeze-25.mp4";
    Check(WriteNumberedSource(source, kRate, kSourceFrames),
          "freeze: a numbered source is generated");
    Document document;
    document.sequence.width = kCodeWidth;
    document.sequence.height = kCodeHeight;
    document.sequence.frame_rate = kRate;
    const Ulid sourceId = "01K5F7W2000000000000000001";
    LibraryMedia media;
    media.id = sourceId;
    media.path = source.string();
    media.filename = source.filename().string();
    media.codec = "h264";
    media.width = kCodeWidth;
    media.height = kCodeHeight;
    media.rate = kRate;
    media.duration = {kSourceFrames, 25};
    media.orientation = "landscape";
    media.has_video = true;
    media.metadata_complete = true;
    document.library.push_back(media);
    document.sources.push_back(
        {sourceId, source.string(), kRate, media.duration});
    const auto clipOf = [&](const Ulid& id, int64_t sourceFrame,
                            int64_t timelineIn, int64_t duration,
                            EffectParamValue speed) {
        DocumentClip clip;
        clip.id = id;
        clip.source_id = sourceId;
        clip.source_in = {sourceFrame, 25};
        clip.duration = {duration, 25};
        clip.timeline_in = {timelineIn, 25};
        clip.include_audio = false;
        clip.speed = speed;
        return clip;
    };
    DocumentTrack track{"01K5F7W2000000000000000010", "video", 0, {}};
    track.clips = {
        clipOf("01K5F7W2000000000000000011", 0, 0, 10, {1, 1}),
        clipOf("01K5F7W2000000000000000012", 37, 10, 50, {0, 1}),
        clipOf("01K5F7W2000000000000000013", 99, 60, 25, {0, 1}),
    };
    document.sequence.tracks = {track};
    std::string validationError;
    Check(document.Validate(validationError),
          "freeze: the timeline validates: " + validationError);

    const auto output = directory / "freeze-export.mp4";
    ExportSettings settings = Exporter::H264Preset(output.string());
    settings.encoder = ExportEncoder::H264Software;
    settings.width = kCodeWidth;
    settings.height = kCodeHeight;
    settings.frame_rate = kRate;
    settings.ffmpeg_path = FFMPEG_EXECUTABLE;
    ExportPlan plan;
    std::string error;
    const bool rendered =
        Exporter::BuildPlan(document, (directory / "freeze.json").string(),
                            settings, plan, error) &&
        Exporter::Run(plan, {}, nullptr, error);
    Check(rendered, "freeze: the timeline exports: " + error);
    std::vector<int> codes;
    Check(rendered && ReadFrameCodes(output.string(), codes),
          "freeze: exported frames decode");
    std::vector<int> expected;
    for (int frame = 0; frame < 10; ++frame) expected.push_back(frame);
    expected.insert(expected.end(), 50, 37);
    expected.insert(expected.end(), 25, 99);
    std::string seen;
    for (size_t index = 0; index < codes.size() && index < 90; ++index) {
        seen += std::to_string(codes[index]) + ' ';
    }
    Check(codes == expected,
          "freeze: 10 frames played, then frame 37 for 50 frames, then the "
          "last frame for 25; got " +
              seen);
}

// R3, criterion 4 -- docs/roadmap/REEL-2026-09.md. Two ramped shots, read
// back frame by frame against ClipSpeed.h's rule. The first is the ticket's
// own: 1 down to 1/4 over 10 frames, held, back to 1 over 10, from a 25 i/s
// rush, opened inside a source frame (a ramp asks nothing of source_in).
// The second runs a 50 i/s rush from 2 down to 1/2 over 20 frames: it
// skips source frames before it holds them, which a list of output times
// has to drop exactly and not by the nearest.
void CheckRampExport(const std::filesystem::path& directory) {
    constexpr MediaRate kRate{25, 1};
    constexpr int kSourceFrames = 200;
    const auto plain = directory / "ramp-25.mp4";
    const auto fast = directory / "ramp-50.mp4";
    Check(WriteNumberedSource(plain, {25, 1}, kSourceFrames) &&
              WriteNumberedSource(fast, {50, 1}, kSourceFrames),
          "ramp: numbered sources are generated");
    Document document;
    document.sequence.width = kCodeWidth;
    document.sequence.height = kCodeHeight;
    document.sequence.frame_rate = kRate;
    const auto describe = [&](const Ulid& id, const std::filesystem::path& path,
                              MediaRate rate) {
        LibraryMedia media;
        media.id = id;
        media.path = path.string();
        media.filename = path.filename().string();
        media.codec = "h264";
        media.width = kCodeWidth;
        media.height = kCodeHeight;
        media.rate = rate;
        media.duration = {kSourceFrames * rate.den, rate.num};
        media.orientation = "landscape";
        media.has_video = true;
        media.metadata_complete = true;
        document.library.push_back(media);
        document.sources.push_back({id, path.string(), rate, media.duration});
    };
    const Ulid plainId = "01K5F7W3000000000000000001";
    const Ulid fastId = "01K5F7W3000000000000000002";
    describe(plainId, plain, {25, 1});
    describe(fastId, fast, {50, 1});
    const auto key = [](const Ulid& id, int64_t frame, EffectParamValue speed,
                        KeyframeInterpolation interpolation =
                            KeyframeInterpolation::Linear) {
        ClipKeyframe keyframe;
        keyframe.id = id;
        keyframe.time = {frame, 25};
        keyframe.value = speed;
        keyframe.interpolation = interpolation;
        return keyframe;
    };
    DocumentClip ticket;
    ticket.id = "01K5F7W3000000000000000011";
    ticket.source_id = plainId;
    ticket.source_in = {21, 50};  // Halfway into source frame 10.
    ticket.duration = {60, 25};
    ticket.timeline_in = {0, 25};
    ticket.include_audio = false;
    ticket.speed_ramp = {key("01K5F7W3000000000000000021", 0, {1, 1}),
                         key("01K5F7W3000000000000000022", 10, {1, 4},
                             KeyframeInterpolation::Hold),
                         key("01K5F7W3000000000000000023", 30, {1, 4}),
                         key("01K5F7W3000000000000000024", 40, {1, 1})};
    DocumentClip braking = ticket;
    braking.id = "01K5F7W3000000000000000012";
    braking.source_id = fastId;
    braking.source_in = {7, 50};
    braking.duration = {30, 25};
    braking.timeline_in = {60, 25};
    braking.speed_ramp = {key("01K5F7W3000000000000000025", 0, {2, 1}),
                          key("01K5F7W3000000000000000026", 20, {1, 2})};
    DocumentTrack track{"01K5F7W3000000000000000010", "video", 0, {}};
    track.clips = {ticket, braking};
    document.sequence.tracks = {track};
    std::string validationError;
    Check(document.Validate(validationError),
          "ramp: the timeline validates: " + validationError);

    const int64_t end = 90;
    const auto output = directory / "ramp-export.mp4";
    ExportSettings settings = Exporter::H264Preset(output.string());
    settings.encoder = ExportEncoder::H264Software;
    settings.width = kCodeWidth;
    settings.height = kCodeHeight;
    settings.frame_rate = kRate;
    settings.ffmpeg_path = FFMPEG_EXECUTABLE;
    ExportPlan plan;
    std::string error;
    const bool rendered =
        Exporter::BuildPlan(document, (directory / "ramp.json").string(),
                            settings, plan, error) &&
        Exporter::Run(plan, {}, nullptr, error);
    Check(rendered, "ramp: the timeline exports: " + error);
    std::vector<int> codes;
    Check(rendered && ReadFrameCodes(output.string(), codes),
          "ramp: exported frames decode");
    Check(codes.size() == static_cast<size_t>(end),
          "ramp: one frame per timeline frame, got " +
              std::to_string(codes.size()));
    int mismatches = 0;
    std::string firstMismatches;
    std::string expectedList;
    for (int64_t frame = 0;
         frame < end && frame < static_cast<int64_t>(codes.size()); ++frame) {
        const DocumentClip& clip = frame < 60
                                       ? document.sequence.tracks[0].clips[0]
                                       : document.sequence.tracks[0].clips[1];
        const DocumentSource* source = document.FindSource(clip.source_id);
        // The rule itself, not a second spelling of it.
        const int64_t expected = ClipSourceFrameAt(
            clip, RationalTime{frame - clip.timeline_in.value, 25},
            source->rate);
        expectedList += std::to_string(expected) + ' ';
        if (codes[frame] != expected) {
            if (mismatches < 8) {
                firstMismatches += " frame " + std::to_string(frame) +
                                   " shows " + std::to_string(codes[frame]) +
                                   " instead of " + std::to_string(expected) +
                                   ';';
            }
            ++mismatches;
        }
    }
    std::cout << "ramp: " << codes.size() << " frames read back, " << mismatches
              << " off the rule\n";
    Check(mismatches == 0,
          "ramp: every exported frame is the source frame the rule names (" +
              std::to_string(mismatches) + " wrong:" + firstMismatches +
              "; expected " + expectedList + ')');

    ExportVerificationSettings verification;
    verification.ffmpeg_path = FFMPEG_EXECUTABLE;
    ExportVerificationReport report;
    Check(VerifyExport(document, directory, output.string(), verification,
                       report, error) &&
              report.expected_frames == end && report.exported_frames == end &&
              report.divergent_frames == 0,
          "ramp: verify_export finds the ramped export exact: " + error + ' ' +
              SerializeExportVerification(report));
    // And a ramp the file does not have is seen: the same file against the
    // first shot at 1/1.
    Document flat = document;
    flat.sequence.tracks[0].clips[0].speed_ramp.clear();
    Check(VerifyExport(flat, directory, output.string(), verification, report,
                       error) &&
              report.divergent_frames > 0,
          "ramp: verify_export sees a ramp the file does not have: " + error +
              ' ' + SerializeExportVerification(report));
}

void CheckSpeedExport(const std::filesystem::path& directory) {
    constexpr MediaRate kRate{25, 1};
    const auto at = [&](int64_t frames) { return RationalTime{frames, 25}; };
    constexpr int kSourceFrames = 400;
    const auto fast = directory / "speed-50.mp4";
    const auto plain = directory / "speed-25.mp4";
    Check(WriteNumberedSource(fast, {50, 1}, kSourceFrames) &&
              WriteNumberedSource(plain, {25, 1}, kSourceFrames),
          "speed: numbered sources are generated");

    Document document;
    document.sequence.width = kCodeWidth;
    document.sequence.height = kCodeHeight;
    document.sequence.frame_rate = kRate;
    const auto describe = [&](const Ulid& id, const std::filesystem::path& path,
                              MediaRate rate) {
        LibraryMedia media;
        media.id = id;
        media.path = path.string();
        media.filename = path.filename().string();
        media.codec = "h264";
        media.width = kCodeWidth;
        media.height = kCodeHeight;
        media.rate = rate;
        media.duration = {kSourceFrames * rate.den, rate.num};
        media.orientation = "landscape";
        media.has_video = true;
        media.has_audio = false;
        media.metadata_complete = true;
        document.library.push_back(media);
        document.sources.push_back({id, path.string(), rate, media.duration});
    };
    const Ulid fastId = "01K5F7W1000000000000000001";
    const Ulid plainId = "01K5F7W1000000000000000002";
    describe(fastId, fast, {50, 1});
    describe(plainId, plain, {25, 1});

    struct Shot {
        Ulid id;
        Ulid source;
        MediaRate source_rate;
        EffectParamValue speed;
        int64_t timeline_in;
        int64_t duration;
        int64_t source_in_frames;  // In source frames.
    };
    // 1/2 of a 50 i/s rush is the film's own case; 1/2 of a 25 i/s rush is
    // the one the editor refused to interpolate and this exports as
    // duplicates; 2 and 2/5 are the ticket's other two.
    const std::vector<Shot> shots = {
        {"01K5F7W1000000000000000011", fastId, {50, 1}, {1, 2}, 0, 20, 60},
        {"01K5F7W1000000000000000012", plainId, {25, 1}, {1, 2}, 20, 20, 30},
        {"01K5F7W1000000000000000013", fastId, {50, 1}, {2, 1}, 40, 20, 0},
        {"01K5F7W1000000000000000014", plainId, {25, 1}, {2, 5}, 60, 20, 10},
    };
    DocumentTrack track{"01K5F7W1000000000000000010", "video", 0, {}};
    for (const Shot& shot : shots) {
        DocumentClip clip;
        clip.id = shot.id;
        clip.source_id = shot.source;
        clip.source_in = {shot.source_in_frames * shot.source_rate.den,
                          shot.source_rate.num};
        clip.duration = at(shot.duration);
        clip.timeline_in = at(shot.timeline_in);
        clip.include_audio = false;
        clip.speed = shot.speed;
        track.clips.push_back(clip);
    }
    document.sequence.tracks = {track};
    std::string validationError;
    Check(document.Validate(validationError),
          "speed: the timeline validates: " + validationError);

    const int64_t end = 80;
    const auto projectPath = directory / "speed.json";
    const auto output = directory / "speed-export.mp4";
    ExportSettings settings = Exporter::H264Preset(output.string());
    settings.encoder = ExportEncoder::H264Software;
    settings.width = kCodeWidth;
    settings.height = kCodeHeight;
    settings.frame_rate = kRate;
    settings.ffmpeg_path = FFMPEG_EXECUTABLE;
    ExportPlan plan;
    std::string error;
    const bool rendered = Exporter::BuildPlan(document, projectPath.string(),
                                              settings, plan, error) &&
                          Exporter::Run(plan, {}, nullptr, error);
    Check(rendered, "speed: the timeline exports: " + error);
    std::vector<int> codes;
    Check(rendered && ReadFrameCodes(output.string(), codes),
          "speed: exported frames decode");
    Check(codes.size() == static_cast<size_t>(end),
          "speed: one frame per timeline frame, got " +
              std::to_string(codes.size()));

    int mismatches = 0;
    std::string firstMismatches;
    for (int64_t frame = 0;
         frame < end && frame < static_cast<int64_t>(codes.size()); ++frame) {
        const DocumentClip* clip = nullptr;
        for (const DocumentClip& candidate :
             document.sequence.tracks[0].clips) {
            const int64_t start = candidate.timeline_in.value;
            if (frame >= start && frame < start + candidate.duration.value)
                clip = &candidate;
        }
        if (clip == nullptr) continue;
        const DocumentSource* source = document.FindSource(clip->source_id);
        // The rule itself, not a second spelling of it.
        const int64_t expected = ClipSourceFrameAt(
            *clip, RationalTime{frame - clip->timeline_in.value, 25},
            source->rate);
        if (codes[frame] != expected) {
            if (mismatches < 8) {
                firstMismatches += " frame " + std::to_string(frame) +
                                   " shows " + std::to_string(codes[frame]) +
                                   " instead of " + std::to_string(expected) +
                                   ';';
            }
            ++mismatches;
        }
    }
    Check(mismatches == 0,
          "speed: every exported frame is the source frame the rule names (" +
              std::to_string(mismatches) + " wrong:" + firstMismatches + ')');

    // Guard rail 1: verify_export resolves the expected picture through
    // Timeline, so it has to find this export exact without knowing
    // anything about speed of its own.
    ExportVerificationSettings verification;
    verification.ffmpeg_path = FFMPEG_EXECUTABLE;
    ExportVerificationReport report;
    Check(VerifyExport(document, directory, output.string(), verification,
                       report, error) &&
              report.expected_frames == end && report.exported_frames == end &&
              report.divergent_frames == 0,
          "speed: verify_export finds the slowed export exact: " + error + ' ' +
              SerializeExportVerification(report));

    // And it must see a wrong speed as wrong: the same file against a
    // document whose first shot is at 1/1 diverges from its second frame on.
    Document wrong = document;
    wrong.sequence.tracks[0].clips[0].speed = {1, 1};
    Check(VerifyExport(wrong, directory, output.string(), verification, report,
                       error) &&
              report.divergent_frames > 0,
          "speed: verify_export sees a speed the file does not have: " + error +
              ' ' + SerializeExportVerification(report));
}

// R3 -- a linked image + sound shot at 1/2 over 22 s of timeline, the
// checklist's listening case measured instead (agentic QA of 23 September
// 2026). The rush's sound is a quiet 440 Hz tone with a loud burst of it at
// every whole source second: slowed, the tone must stay at 440 Hz (atempo,
// not a resampling that would drop it an octave), and burst n must start at
// 2n seconds of timeline, the last one included, within a frame.
void CheckSlowedAudioExport(const std::filesystem::path& directory) {
    const auto source = directory / "slowed-sound.mov";
    const std::string generate =
        Quote(FFMPEG_EXECUTABLE) +
        " -hide_banner -loglevel error "
        "-f lavfi -i 'testsrc2=size=64x36:rate=25:duration=12' "
        "-f lavfi -i \"aevalsrc='(0.2+0.7*lt(mod(t\\,1)\\,0.05))*"
        "sin(2*PI*440*t)':s=48000:d=12\" "
        "-c:v libx264 -pix_fmt yuv420p -c:a pcm_s16le -shortest -y " +
        Quote(source.string());
    Check(std::system(generate.c_str()) == 0,
          "slowed sound: the rush is generated");

    Document document;
    document.sequence.width = 64;
    document.sequence.height = 36;
    document.sequence.frame_rate = {25, 1};
    LibraryMedia media;
    media.id = "01K5F7W2000000000000000001";
    media.path = source.string();
    media.filename = source.filename().string();
    media.codec = "h264";
    media.width = 64;
    media.height = 36;
    media.rate = {25, 1};
    media.duration = {300, 25};
    media.orientation = "landscape";
    media.has_video = true;
    media.has_audio = true;
    media.audio_rate = 48000;
    media.audio_channels = 1;
    media.metadata_complete = true;
    document.library = {media};
    document.sources = {{media.id, source.string(), {25, 1}, {300, 25}}};
    DocumentClip picture;
    picture.id = "01K5F7W2000000000000000011";
    picture.source_id = media.id;
    picture.source_in = {0, 25};
    picture.duration = {550, 25};
    picture.timeline_in = {0, 25};
    picture.include_audio = false;
    picture.speed = {1, 2};
    picture.link_group_id = "01K5F7W2000000000000000020";
    DocumentClip sound = picture;
    sound.id = "01K5F7W2000000000000000012";
    sound.include_audio = true;
    document.sequence.tracks = {
        {"01K5F7W2000000000000000030", "video", 0, {picture}},
        {"01K5F7W2000000000000000031", "audio", 1, {sound}},
    };
    std::string error;
    Check(document.Validate(error),
          "slowed sound: the timeline validates: " + error);

    const auto output = directory / "slowed-sound-export.mp4";
    ExportSettings settings = Exporter::H264Preset(output.string());
    settings.encoder = ExportEncoder::H264Software;
    settings.width = 64;
    settings.height = 36;
    settings.frame_rate = {25, 1};
    settings.ffmpeg_path = FFMPEG_EXECUTABLE;
    ExportPlan plan;
    const bool rendered =
        Exporter::BuildPlan(document, (directory / "slowed.json").string(),
                            settings, plan, error) &&
        Exporter::Run(plan, {}, nullptr, error);
    Check(rendered, "slowed sound: the timeline exports: " + error);
    if (!rendered) return;

    const auto samplesPath = directory / "slowed-sound.f32";
    const std::string decode =
        Quote(FFMPEG_EXECUTABLE) + " -hide_banner -loglevel error -i " +
        Quote(output.string()) + " -map 0:a:0 -ac 1 -ar 48000 -f f32le -y " +
        Quote(samplesPath.string());
    std::vector<float> samples;
    if (std::system(decode.c_str()) == 0) {
        std::ifstream input(samplesPath, std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(input)),
                                std::istreambuf_iterator<char>());
        samples.resize(bytes.size() / sizeof(float));
        std::memcpy(samples.data(), bytes.data(),
                    samples.size() * sizeof(float));
    }
    constexpr int kRate = 48000;
    Check(samples.size() >= static_cast<size_t>(21 * kRate),
          "slowed sound: 22 s of sound decode, got " +
              std::to_string(samples.size() / kRate) + " s");
    if (samples.size() < static_cast<size_t>(21 * kRate)) return;

    // The pitch, between two bursts: zero crossings of the quiet tone over
    // 0.8 s starting 0.5 s into the shot.
    int crossings = 0;
    for (int index = kRate / 2 + 1; index < kRate / 2 + (kRate * 8 / 10);
         ++index)
        crossings += (samples[index - 1] < 0.0f) != (samples[index] < 0.0f);
    const double pitch = crossings / 2.0 / 0.8;
    Check(std::abs(pitch - 440.0) <= 5.0,
          "slowed sound: the tone stays at 440 Hz, pitch kept, got " +
              std::to_string(pitch));

    // Burst onsets: the 5 ms window whose loudness first crosses halfway
    // between the tone and the burst.
    constexpr int kWindow = kRate / 200;
    std::vector<double> onsets;
    bool loud = false;
    for (size_t start = 0; start + kWindow <= samples.size();
         start += kWindow) {
        double energy = 0.0;
        for (int index = 0; index < kWindow; ++index)
            energy += samples[start + index] * samples[start + index];
        const double rms = std::sqrt(energy / kWindow);
        if (!loud && rms > 0.3) onsets.push_back(double(start) / kRate);
        loud = rms > 0.3 ? true : (rms < 0.2 ? false : loud);
    }
    std::string report;
    double worst = 0.0;
    for (size_t index = 0; index < onsets.size(); ++index) {
        worst = std::max(worst, std::abs(onsets[index] - (2.0 * index)));
        report += ' ' + std::to_string(onsets[index]).substr(0, 6);
    }
    std::cout << "slowed sound: " << pitch << " Hz, bursts at" << report
              << " s\n";
    Check(onsets.size() == 11 && worst <= 0.04,
          "slowed sound: bursts 0 to 10 start at 0, 2 ... 20 s of timeline, "
          "within a frame to the last (worst " +
              std::to_string(worst) + " s):" + report);
}

// R1 -- docs/roadmap/REEL-2026-09.md, criterion 1. A 1920x1080 rush filled
// into a 1080x1920 sequence at three horizontal positions: no black bar,
// and the picture delivered is the source region ClipFraming.h named,
// compared pixel to pixel against that region rendered on its own.
//
// The source is testsrc2 pushed into [64, 224], so that it has structure
// everywhere (a misregistration of a few pixels shows) and no black pixel
// anywhere (a black row in the delivery can only be a bar).
//
// Everything is compared on the Y plane of a yuv420p decode, never through
// `-pix_fmt gray`: gray is a colour conversion, and the delivered file is
// tagged BT.709 while a raw crop of the rush is not, so the two would be
// converted differently and a colour difference would be read as a
// geometry one. That cost an hour; it is written down here so it does not
// cost the next one.
void CheckFramingExport(const std::filesystem::path& directory) {
    constexpr int kSourceWidth = 1920;
    constexpr int kSourceHeight = 1080;
    constexpr int kFrameWidth = 1080;
    constexpr int kFrameHeight = 1920;
    const auto rush = directory / "framing-rush.mp4";
    const std::string generate =
        Quote(FFMPEG_EXECUTABLE) +
        " -hide_banner -loglevel error -f lavfi -i "
        "'testsrc2=size=1920x1080:rate=25:duration=0.4' -vf "
        "\"lutyuv=y='64+val*160/255',format=yuv420p\" -c:v libx264 -crf 0 "
        "-y " +
        Quote(rush.string());
    Check(std::system(generate.c_str()) == 0, "framing: rush is generated");

    const size_t lumaBytes = static_cast<size_t>(kFrameWidth) * kFrameHeight;
    const size_t frameBytes = lumaBytes * 3 / 2;
    const auto readRaw = [](const std::filesystem::path& path,
                            std::vector<unsigned char>& pixels) {
        std::ifstream input(path, std::ios::binary);
        pixels.assign(std::istreambuf_iterator<char>(input),
                      std::istreambuf_iterator<char>());
        return !pixels.empty();
    };

    const auto buildDocument = [&](bool filled) {
        Document document;
        document.sequence.width = kFrameWidth;
        document.sequence.height = kFrameHeight;
        document.sequence.frame_rate = {25, 1};
        const Ulid sourceId = "01K6F7W1000000000000000001";
        LibraryMedia media;
        media.id = sourceId;
        media.path = rush.string();
        media.filename = "framing-rush.mp4";
        media.codec = "h264";
        media.orientation = "landscape";
        media.has_video = true;
        media.width = kSourceWidth;
        media.height = kSourceHeight;
        media.rate = {25, 1};
        media.duration = {10, 25};
        media.metadata_complete = true;
        document.library.push_back(media);
        document.sources.push_back(
            {sourceId, rush.string(), {25, 1}, {10, 25}});
        const EffectParamValue positions[3] = {{-1, 4}, {0, 1}, {1, 4}};
        DocumentTrack track{"01K6F7W10000000000000000T0", "video", 0, {}};
        for (int index = 0; index < 3; ++index) {
            DocumentClip clip;
            clip.id = "01K6F7W10000000000000000C" + std::to_string(index);
            clip.source_id = sourceId;
            clip.source_in = {0, 25};
            clip.duration = {2, 25};
            clip.timeline_in = {2 * index, 25};
            clip.include_audio = false;
            if (filled) {
                clip.framing.mode = FramingMode::Fill;
                clip.framing.position_x = positions[index];
            }
            track.clips.push_back(clip);
        }
        document.sequence.tracks = {track};
        return document;
    };

    const auto render = [&](const Document& document, const std::string& name,
                            std::vector<unsigned char>& pixels) {
        const auto output = directory / (name + ".mp4");
        ExportSettings settings = Exporter::H264Preset(output.string());
        settings.encoder = ExportEncoder::H264Software;
        settings.width = kFrameWidth;
        settings.height = kFrameHeight;
        settings.frame_rate = {25, 1};
        settings.ffmpeg_path = FFMPEG_EXECUTABLE;
        // Near-lossless: what is compared below is a geometry of a few
        // pixels, and the encoder's own noise must stay far under it.
        settings.video_bitrate = 200000000;
        ExportPlan plan;
        std::string error;
        if (!Exporter::BuildPlan(document,
                                 (directory / (name + ".json")).string(),
                                 settings, plan, error) ||
            !Exporter::Run(plan, {}, nullptr, error)) {
            Check(false, "framing: " + name + " exports: " + error);
            return false;
        }
        const auto raw = directory / (name + ".yuv");
        const std::string decode =
            Quote(FFMPEG_EXECUTABLE) + " -hide_banner -loglevel error -i " +
            Quote(output.string()) +
            " -map 0:v:0 -an -sn -f rawvideo -pix_fmt yuv420p -y " +
            Quote(raw.string());
        Check(std::system(decode.c_str()) == 0,
              "framing: " + name + " decodes");
        return readRaw(raw, pixels) && pixels.size() == frameBytes * 6;
    };

    const Document filled = buildDocument(true);
    std::string validationError;
    Check(filled.Validate(validationError),
          "framing: the filled timeline validates: " + validationError);
    std::vector<unsigned char> delivered;
    if (!render(filled, "framing-fill", delivered)) return;

    // The control: the same timeline with the framing every clip had before
    // this ticket. It must show the bars the filled one does not.
    std::vector<unsigned char> fitted;
    if (!render(buildDocument(false), "framing-fit", fitted)) return;

    const auto darkestRowMean = [&](const std::vector<unsigned char>& pixels,
                                    size_t frame) {
        const unsigned char* data = pixels.data() + (frameBytes * frame);
        double darkest = 1e9;
        for (int y = 0; y < kFrameHeight; ++y) {
            double sum = 0.0;
            for (int x = 0; x < kFrameWidth; ++x)
                sum += data[(static_cast<size_t>(y) * kFrameWidth) + x];
            darkest = std::min(darkest, sum / kFrameWidth);
        }
        return darkest;
    };
    Check(darkestRowMean(fitted, 0) < 24.0,
          "framing: fitted, the delivery has a black bar (darkest row " +
              std::to_string(darkestRowMean(fitted, 0)) + ')');

    for (int index = 0; index < 3; ++index) {
        const DocumentClip& clip = filled.sequence.tracks[0].clips[index];
        ClipFramingGeometry geometry;
        std::string error;
        Check(ResolveClipFramingForClip(filled, clip, kFrameWidth, kFrameHeight,
                                        geometry, error),
              "framing: the rectangle resolves: " + error);
        Check(geometry.covers_frame, "framing: clip " + std::to_string(index) +
                                         " covers the frame by the rule");
        const double darkest =
            darkestRowMean(delivered, static_cast<size_t>(index) * 2);
        Check(darkest > 56.0, "framing: no black bar in clip " +
                                  std::to_string(index) + " (darkest row " +
                                  std::to_string(darkest) + ')');

        // The region the rule named, rendered on its own out of the rush,
        // and compared with what the export delivered.
        const auto reference =
            directory / ("framing-region-" + std::to_string(index) + ".yuv");
        const std::string cut =
            Quote(FFMPEG_EXECUTABLE) + " -hide_banner -loglevel error -i " +
            Quote(rush.string()) + " -map 0:v:0 -frames:v 1 -vf \"crop=" +
            std::to_string(geometry.source_width) + ':' +
            std::to_string(geometry.source_height) + ':' +
            std::to_string(geometry.source_x) + ':' +
            std::to_string(geometry.source_y) +
            ",scale=" + std::to_string(geometry.width) + ':' +
            std::to_string(geometry.height) +
            ":flags=lanczos\" -f rawvideo -pix_fmt yuv420p -y " +
            Quote(reference.string());
        Check(std::system(cut.c_str()) == 0,
              "framing: the expected region renders");
        std::vector<unsigned char> expected;
        Check(readRaw(reference, expected) && expected.size() == frameBytes,
              "framing: the expected region is one frame");
        if (expected.size() != frameBytes) continue;
        const auto meanDifference = [&](size_t frame) {
            const unsigned char* data = delivered.data() + (frameBytes * frame);
            double sum = 0.0;
            for (size_t offset = 0; offset < lumaBytes; ++offset)
                sum += std::abs(static_cast<int>(data[offset]) -
                                static_cast<int>(expected[offset]));
            return sum / static_cast<double>(lumaBytes);
        };
        const double match = meanDifference(static_cast<size_t>(index) * 2);
        Check(match < 2.0,
              "framing: clip " + std::to_string(index) +
                  " delivers the region the rule named (mean difference " +
                  std::to_string(match) + ", crop " +
                  std::to_string(geometry.source_width) + 'x' +
                  std::to_string(geometry.source_height) + '+' +
                  std::to_string(geometry.source_x) + '+' +
                  std::to_string(geometry.source_y) + ')');
        // And it is the region of THIS position, not of another: the same
        // comparison against a neighbour's frame must be far worse, or the
        // check above would pass on any picture.
        const size_t other = index == 0 ? 4 : 0;
        Check(meanDifference(other) > 20.0,
              "framing: clip " + std::to_string(index) +
                  " is not the picture of another position (" +
                  std::to_string(meanDifference(other)) + " against " +
                  std::to_string(match) + ')');
    }
}

// R2 -- docs/roadmap/REEL-2026-09.md, criterion 3. A zoom of 100 % to
// 115 % in cubic-bezier over 12 frames exports, and reads back at three
// frames, against the engine's own evaluation.
//
// The rush is a single picture repeated, on purpose: at speed 1/1 every
// output frame then shows the same source picture, so the only thing that
// can differ between one delivered frame and the next is the geometry --
// which is what this measures. A moving source would hide a wrong
// rectangle behind a changed picture.
//
// Everything is compared on the Y plane of a yuv420p decode, for the reason
// CheckFramingExport writes down: `-pix_fmt gray` is a colour conversion
// and the two sides are tagged differently.
void CheckAnimatedFramingExport(const std::filesystem::path& directory) {
    constexpr int kSourceWidth = 1920;
    constexpr int kSourceHeight = 1080;
    constexpr int kFrameWidth = 1080;
    constexpr int kFrameHeight = 1920;
    constexpr int kFrames = 12;
    const auto still = directory / "animation-still.png";
    const auto rush = directory / "animation-rush.mp4";
    const std::string makeStill =
        Quote(FFMPEG_EXECUTABLE) +
        " -hide_banner -loglevel error -f lavfi -i "
        "'testsrc2=size=1920x1080:rate=25:duration=0.04' -frames:v 1 -y " +
        Quote(still.string());
    Check(std::system(makeStill.c_str()) == 0, "animation: the still renders");
    const std::string generate =
        Quote(FFMPEG_EXECUTABLE) +
        " -hide_banner -loglevel error -loop 1 -framerate 25 -i " +
        Quote(still.string()) +
        " -t 0.6 -vf \"lutyuv=y='64+val*160/255',format=yuv420p\" -c:v "
        "libx264 -crf 0 -y " +
        Quote(rush.string());
    Check(std::system(generate.c_str()) == 0, "animation: the rush renders");

    Document document;
    document.sequence.width = kFrameWidth;
    document.sequence.height = kFrameHeight;
    document.sequence.frame_rate = {25, 1};
    const Ulid sourceId = "01K9A000000000000000000001";
    LibraryMedia media;
    media.id = sourceId;
    media.path = rush.string();
    media.filename = "animation-rush.mp4";
    media.codec = "h264";
    media.orientation = "landscape";
    media.has_video = true;
    media.width = kSourceWidth;
    media.height = kSourceHeight;
    media.rate = {25, 1};
    media.duration = {15, 25};
    media.metadata_complete = true;
    document.library.push_back(media);
    document.sources.push_back({sourceId, rush.string(), {25, 1}, {15, 25}});
    DocumentClip clip;
    clip.id = "01K9A00000000000000000000C";
    clip.source_id = sourceId;
    clip.source_in = {0, 25};
    clip.duration = {kFrames, 25};
    clip.timeline_in = {0, 25};
    clip.include_audio = false;
    clip.framing.mode = FramingMode::Fill;
    ClipKeyframe first;
    first.id = "01K9A0000000000000000000K1";
    first.time = {0, 25};
    first.value = EffectParamValue{1, 1};
    first.interpolation = KeyframeInterpolation::CubicBezier;
    first.easing_x1 = EffectParamValue{2, 5};
    first.easing_y1 = EffectParamValue{0, 1};
    first.easing_x2 = EffectParamValue{1, 5};
    first.easing_y2 = EffectParamValue{1, 1};
    ClipKeyframe last;
    last.id = "01K9A0000000000000000000K2";
    last.time = {kFrames, 25};
    last.value = EffectParamValue{23, 20};
    ClipParameterAnimation zoom;
    zoom.parameter = ClipAnimatedParameter::FramingScale;
    zoom.keyframes = {first, last};
    clip.animation = {zoom};
    document.sequence.tracks = {
        DocumentTrack{"01K9A0000000000000000000T0", "video", 0, {clip}}};
    std::string validationError;
    Check(document.Validate(validationError),
          "animation: the timeline validates: " + validationError);

    const auto output = directory / "animation-zoom.mp4";
    ExportSettings settings = Exporter::H264Preset(output.string());
    settings.encoder = ExportEncoder::H264Software;
    settings.width = kFrameWidth;
    settings.height = kFrameHeight;
    settings.frame_rate = {25, 1};
    settings.ffmpeg_path = FFMPEG_EXECUTABLE;
    settings.video_bitrate = 200000000;
    ExportPlan plan;
    std::string error;
    if (!Exporter::BuildPlan(document, (directory / "animation.json").string(),
                             settings, plan, error) ||
        !Exporter::Run(plan, {}, nullptr, error)) {
        Check(false, "animation: the zoom exports: " + error);
        return;
    }
    const size_t lumaBytes = static_cast<size_t>(kFrameWidth) * kFrameHeight;
    const size_t frameBytes = lumaBytes * 3 / 2;
    const auto readRaw = [](const std::filesystem::path& path,
                            std::vector<unsigned char>& pixels) {
        std::ifstream input(path, std::ios::binary);
        pixels.assign(std::istreambuf_iterator<char>(input),
                      std::istreambuf_iterator<char>());
        return !pixels.empty();
    };
    const auto raw = directory / "animation-zoom.yuv";
    const std::string decode =
        Quote(FFMPEG_EXECUTABLE) + " -hide_banner -loglevel error -i " +
        Quote(output.string()) +
        " -map 0:v:0 -an -sn -f rawvideo -pix_fmt yuv420p -y " +
        Quote(raw.string());
    Check(std::system(decode.c_str()) == 0, "animation: the delivery decodes");
    std::vector<unsigned char> delivered;
    Check(readRaw(raw, delivered) && delivered.size() == frameBytes * kFrames,
          "animation: the delivery is " + std::to_string(kFrames) + " frames");
    if (delivered.size() != frameBytes * kFrames) return;

    // The engine's own evaluation, at three frames: the head, the middle
    // and the last. Each is rendered on its own out of the still, through
    // the rectangle ClipAnimation.h named, and compared with what came out
    // of the export.
    const int64_t probes[3] = {0, 6, kFrames - 1};
    std::vector<unsigned char> firstReference;
    for (int index = 0; index < 3; ++index) {
        const int64_t frame = probes[index];
        ClipFramingGeometry geometry;
        EffectParamValue opacity{1, 1};
        std::string sampleError;
        Check(ResolveClipFramingAtFrame(
                  document, document.sequence.tracks[0].clips[0], frame,
                  kFrameWidth, kFrameHeight, geometry, opacity, sampleError),
              "animation: the rectangle of frame " + std::to_string(frame) +
                  " resolves: " + sampleError);
        Check(geometry.covers_frame, "animation: frame " +
                                         std::to_string(frame) +
                                         " covers the frame by the rule");
        const auto reference =
            directory / ("animation-ref-" + std::to_string(frame) + ".yuv");
        const std::string render =
            Quote(FFMPEG_EXECUTABLE) + " -hide_banner -loglevel error -i " +
            Quote(rush.string()) + " -map 0:v:0 -frames:v 1 -vf \"scale=" +
            std::to_string(geometry.full_width) + ':' +
            std::to_string(geometry.full_height) +
            ":flags=lanczos,crop=" + std::to_string(kFrameWidth) + ':' +
            std::to_string(kFrameHeight) + ':' +
            std::to_string(-geometry.full_x) + ':' +
            std::to_string(-geometry.full_y) +
            "\" -f rawvideo -pix_fmt "
            "yuv420p -y " +
            Quote(reference.string());
        Check(std::system(render.c_str()) == 0,
              "animation: the expected picture of frame " +
                  std::to_string(frame) + " renders");
        std::vector<unsigned char> expected;
        Check(readRaw(reference, expected) && expected.size() == frameBytes,
              "animation: the expected picture is one frame");
        if (expected.size() != frameBytes) continue;
        if (index == 0) firstReference = expected;
        const auto meanDifference = [&](const std::vector<unsigned char>& left,
                                        size_t at) {
            const unsigned char* data = delivered.data() + (frameBytes * at);
            double sum = 0.0;
            for (size_t offset = 0; offset < lumaBytes; ++offset)
                sum += std::abs(static_cast<int>(data[offset]) -
                                static_cast<int>(left[offset]));
            return sum / static_cast<double>(lumaBytes);
        };
        const double match =
            meanDifference(expected, static_cast<size_t>(frame));
        Check(match < 2.0,
              "animation: frame " + std::to_string(frame) +
                  " is the picture the engine evaluated (mean difference " +
                  std::to_string(match) + ", " +
                  std::to_string(geometry.full_width) + 'x' +
                  std::to_string(geometry.full_height) + " at " +
                  std::to_string(geometry.full_x) + ',' +
                  std::to_string(geometry.full_y) + ')');
        // And it is THIS frame's zoom, not the first one's. An export that
        // ignored the keys and drew every frame at the head's rectangle
        // would land here instead, and the comparison above would already
        // fail -- this states the margin it fails by, so a later change
        // that narrows it says so.
        if (index == 2 && !firstReference.empty()) {
            const double against =
                meanDifference(firstReference, static_cast<size_t>(frame));
            Check(against > 1.0 && against > 4.0 * match,
                  "animation: the last frame is not the first frame's "
                  "rectangle (" +
                      std::to_string(against) + " against " +
                      std::to_string(match) + ')');
        }
    }
}

// IA4 -- docs/roadmap/IA-2026-09.md. A window of the timeline is rendered
// by the same graph as the film, so it must BE the film on those frames: the
// same bytes, not merely a close picture. Two layers, a translucent one that
// zooms, a cross dissolve, a closing dip, a caption, and sound with a gain
// and fades -- everything an inspection around a cut has to show -- rendered
// without loss in full and as a window, then compared frame for frame and
// sample for sample, without colour management and with it.
void CheckWindowRender(const std::filesystem::path& directory) {
    constexpr int kWidth = 320;
    constexpr int kHeight = 180;
    constexpr int64_t kFirst = 40;
    constexpr int64_t kEnd = 70;
    const auto rushA = directory / "window-a.mov";
    const auto rushB = directory / "window-b.mov";
    const auto makeRush = [&](const std::filesystem::path& path,
                              const char* pattern, int tone) {
        return std::system(
                   (Quote(FFMPEG_EXECUTABLE) +
                    " -hide_banner -loglevel error -f lavfi -i "
                    "'" +
                    pattern +
                    "=size=320x180:rate=25:duration=4' -f lavfi -i "
                    "'sine=frequency=" +
                    std::to_string(tone) +
                    ":sample_rate=48000:duration=4' -ac 2 -c:v "
                    "libx264 -crf 0 -pix_fmt yuv420p -c:a pcm_s16le -y " +
                    Quote(path.string()))
                       .c_str()) == 0;
    };
    Check(makeRush(rushA, "testsrc2", 440) && makeRush(rushB, "testsrc", 880),
          "window: the rushes render");
    Document document;
    document.sequence.width = kWidth;
    document.sequence.height = kHeight;
    document.sequence.frame_rate = {25, 1};
    const auto addMedia = [&](const Ulid& id,
                              const std::filesystem::path& path) {
        LibraryMedia media;
        media.id = id;
        media.path = path.string();
        media.filename = path.filename().string();
        media.codec = "h264";
        media.orientation = "landscape";
        media.has_video = true;
        media.has_audio = true;
        media.audio_rate = 48000;
        media.audio_channels = 2;
        media.width = kWidth;
        media.height = kHeight;
        media.rate = {25, 1};
        media.duration = {100, 25};
        media.metadata_complete = true;
        document.library.push_back(media);
        document.sources.push_back({id, path.string(), {25, 1}, {100, 25}});
    };
    addMedia("01K9F000000000000000000001", rushA);
    addMedia("01K9F000000000000000000002", rushB);
    const auto clipOf = [](const char* id, const char* source, int64_t in,
                           int64_t length, int64_t at) {
        DocumentClip clip;
        clip.id = id;
        clip.source_id = source;
        clip.source_in = {in, 25};
        clip.duration = {length, 25};
        clip.timeline_in = {at, 25};
        clip.include_audio = false;
        return clip;
    };
    DocumentClip first = clipOf("01K9F00000000000000000000A",
                                "01K9F000000000000000000001", 0, 50, 0);
    DocumentClip second = clipOf("01K9F00000000000000000000B",
                                 "01K9F000000000000000000002", 10, 50, 50);
    DocumentClip over = clipOf("01K9F00000000000000000000C",
                               "01K9F000000000000000000002", 0, 40, 30);
    over.opacity = {1, 2};
    over.framing.mode = FramingMode::Fill;
    ClipKeyframe wide;
    wide.id = "01K9F0000000000000000000K1";
    wide.time = {0, 25};
    wide.value = EffectParamValue{1, 1};
    ClipKeyframe close;
    close.id = "01K9F0000000000000000000K2";
    close.time = {39, 25};
    close.value = EffectParamValue{3, 2};
    ClipParameterAnimation zoom;
    zoom.parameter = ClipAnimatedParameter::FramingScale;
    zoom.keyframes = {wide, close};
    over.animation = {zoom};
    DocumentClip soundA = clipOf("01K9F00000000000000000000D",
                                 "01K9F000000000000000000001", 0, 50, 0);
    soundA.audio_gain_db = {-6, 1};
    soundA.audio_fade_in = {5, 25};
    DocumentClip soundB = clipOf("01K9F00000000000000000000E",
                                 "01K9F000000000000000000002", 10, 50, 50);
    soundB.audio_fade_out = {10, 25};
    document.sequence.caption_styles = {
        {"01K9F0000000000000000000S1", "Sans", 18, "#ffffff", "bottom"}};
    DocumentClip caption;
    caption.id = "01K9F0000000000000000000C1";
    caption.source_in = {0, 25};
    caption.timeline_in = {45, 25};
    caption.duration = {15, 25};
    caption.include_audio = false;
    caption.caption_group_id = "01K9F0000000000000000000S1";
    caption.caption_text = "Fenêtre";
    document.sequence.tracks = {
        DocumentTrack{
            "01K9F0000000000000000000T0", "video", 0, {first, second}},
        DocumentTrack{"01K9F0000000000000000000T1", "video", 1, {over}},
        DocumentTrack{
            "01K9F0000000000000000000T2", "audio", 2, {soundA, soundB}},
        DocumentTrack{"01K9F0000000000000000000T3", "caption", 3, {caption}}};
    DocumentTransition dissolve;
    dissolve.id = "01K9F0000000000000000000X1";
    dissolve.track_id = "01K9F0000000000000000000T0";
    dissolve.left_clip_id = first.id;
    dissolve.right_clip_id = second.id;
    dissolve.type = "cross_dissolve";
    dissolve.duration = {10, 25};
    dissolve.alignment = TransitionAlignment::Center;
    DocumentTransition closing;
    closing.id = "01K9F0000000000000000000X2";
    closing.track_id = "01K9F0000000000000000000T0";
    closing.left_clip_id = second.id;
    closing.type = "dip_to_color";
    closing.color = "#000000";
    closing.duration = {12, 25};
    closing.alignment = TransitionAlignment::EndAtCut;
    document.sequence.transitions = {dissolve, closing};
    std::string error;
    Check(document.Validate(error), "window: the timeline validates: " + error);

    const auto render =
        [&](const Document& film, const std::string& name, int64_t first,
            int64_t end, std::vector<int64_t> keep = {},
            int32_t inputsPerPass = 0, size_t* segments = nullptr,
            std::vector<ExportProgress>* progressLog = nullptr) {
            ExportSettings settings = Exporter::HevcMain10SoftwarePreset(
                (directory / (name + ".mkv")).string());
            settings.encoder = ExportEncoder::LosslessInspection;
            settings.width = kWidth;
            settings.height = kHeight;
            settings.frame_rate = {25, 1};
            settings.ffmpeg_path = FFMPEG_EXECUTABLE;
            settings.overwrite = true;
            settings.window_first = first;
            settings.window_end = end;
            settings.inspection_frames = std::move(keep);
            settings.max_inputs_per_pass = inputsPerPass;
            ExportPlan plan;
            std::string renderError;
            const bool planned =
                Exporter::BuildPlan(film, (directory / "window.json").string(),
                                    settings, plan, renderError);
            if (segments != nullptr) *segments = plan.segments.size();
            std::mutex progressMutex;
            ExportProgressCallback collect;
            if (progressLog != nullptr) {
                collect = [&](const ExportProgress& progress) {
                    std::lock_guard<std::mutex> lock(progressMutex);
                    progressLog->push_back(progress);
                };
            }
            const bool rendered =
                planned && Exporter::Run(plan, collect, nullptr, renderError);
            Check(rendered, "window: " + name + " renders: " + renderError);
            return rendered;
        };
    const auto readStream = [&](const std::string& name, bool audio) {
        const auto raw = directory / (name + (audio ? ".f32" : ".yuv"));
        const std::string command =
            Quote(FFMPEG_EXECUTABLE) + " -hide_banner -loglevel error -i " +
            Quote((directory / (name + ".mkv")).string()) +
            (audio ? " -map 0:a:0 -f f32le -y "
                   : " -map 0:v:0 -fps_mode passthrough -f rawvideo -pix_fmt "
                     "yuv420p10le -y ") +
            Quote(raw.string());
        std::vector<char> bytes;
        if (std::system(command.c_str()) == 0) {
            std::ifstream input(raw, std::ios::binary);
            bytes.assign(std::istreambuf_iterator<char>(input),
                         std::istreambuf_iterator<char>());
        }
        return bytes;
    };
    const size_t frameBytes = static_cast<size_t>(kWidth) * kHeight * 3;
    const size_t samplesPerFrame = 48000 / 25 * 2;
    for (const bool managed : {false, true}) {
        Document film = document;
        film.color_management.enabled = managed;
        const std::string full =
            managed ? "window-full-managed" : "window-full";
        const std::string part =
            managed ? "window-part-managed" : "window-part";
        if (!render(film, full, 0, 0) || !render(film, part, kFirst, kEnd))
            continue;
        const std::vector<char> fullVideo = readStream(full, false);
        const std::vector<char> partVideo = readStream(part, false);
        Check(fullVideo.size() == frameBytes * 100 &&
                  partVideo.size() == frameBytes * (kEnd - kFirst),
              "window: " + part + " holds exactly its " +
                  std::to_string(kEnd - kFirst) + " frames (" +
                  std::to_string(partVideo.size() / frameBytes) + ")");
        if (fullVideo.size() == frameBytes * 100 &&
            partVideo.size() == frameBytes * (kEnd - kFirst)) {
            int64_t differing = 0;
            for (int64_t frame = 0; frame < kEnd - kFirst; ++frame) {
                if (std::memcmp(
                        partVideo.data() + (frameBytes * frame),
                        fullVideo.data() + (frameBytes * (kFirst + frame)),
                        frameBytes) != 0)
                    ++differing;
            }
            Check(differing == 0, "window: every frame of " + part +
                                      " is the film's frame, byte for byte (" +
                                      std::to_string(differing) + " differ)");
        }
        const std::vector<char> fullAudio = readStream(full, true);
        const std::vector<char> partAudio = readStream(part, true);
        const size_t partSamples = partAudio.size() / sizeof(float);
        Check(partSamples == samplesPerFrame * (kEnd - kFirst),
              "window: " + part + " holds exactly its samples (" +
                  std::to_string(partSamples) + ")");
        if (partSamples == samplesPerFrame * (kEnd - kFirst) &&
            fullAudio.size() / sizeof(float) >= samplesPerFrame * kEnd) {
            const auto* whole =
                reinterpret_cast<const float*>(fullAudio.data());
            const auto* window =
                reinterpret_cast<const float*>(partAudio.data());
            double worst = 0.0;
            for (size_t index = 0; index < partSamples; ++index)
                worst = std::max(
                    worst, static_cast<double>(std::abs(
                               window[index] -
                               whole[(samplesPerFrame * kFirst) + index])));
            Check(worst <= 1e-6, "window: the sound of " + part +
                                     " is the film's, sample for sample "
                                     "(worst " +
                                     std::to_string(worst) + ")");
        }
        // Rendering by segments: the film forced into passes of at most
        // three inputs -- windows cut inside the dissolve, the zoom and the
        // caption -- is the single pass, frame for frame and sample for
        // sample. The segments are gone afterwards.
        const std::string segmented =
            managed ? "window-segmented-managed" : "window-segmented";
        size_t segmentCount = 0;
        std::vector<ExportProgress> passes;
        if (render(film, segmented, 0, 0, {}, 3, &segmentCount, &passes)) {
            Check(segmentCount > 1, "window: " + segmented +
                                        " is rendered in several segments (" +
                                        std::to_string(segmentCount) + ")");
            // PRD40 -- each pass says its name and counts the film's frames,
            // never the two passes' sum.
            bool sawSegment = false;
            bool sawAssembly = false;
            bool counted = !passes.empty();
            for (const ExportProgress& progress : passes) {
                sawSegment |= progress.pass.rfind("segment 1/", 0) == 0;
                sawAssembly |= progress.pass == "assembly";
                counted = counted && !progress.pass.empty() &&
                          progress.pass_total * 2 == progress.total_frames &&
                          progress.pass_frames <= progress.pass_total;
            }
            Check(sawSegment && sawAssembly && counted &&
                      passes.back().pass == "assembly",
                  "window: " + segmented +
                      " reports its segments, then its assembly, each "
                      "against the film's frames (" +
                      std::to_string(passes.size()) + " reports)");
            const std::vector<char> segmentedVideo =
                readStream(segmented, false);
            std::string differing;
            int worstLevel = 0;
            if (segmentedVideo.size() == fullVideo.size()) {
                for (size_t frame = 0; frame < fullVideo.size() / frameBytes;
                     ++frame) {
                    const auto* x = reinterpret_cast<const uint16_t*>(
                        segmentedVideo.data() + (frameBytes * frame));
                    const auto* y = reinterpret_cast<const uint16_t*>(
                        fullVideo.data() + (frameBytes * frame));
                    int level = 0;
                    for (size_t at = 0; at < frameBytes / 2; ++at)
                        level =
                            std::max(level, std::abs(int(x[at]) - int(y[at])));
                    if (level > 0) {
                        differing += " " + std::to_string(frame) + ":" +
                                     std::to_string(level);
                        worstLevel = std::max(worstLevel, level);
                    }
                }
            }
            Check(
                segmentedVideo.size() == fullVideo.size() && differing.empty(),
                "window: " + segmented +
                    " is the single pass, byte for byte (" +
                    std::to_string(segmentedVideo.size() / frameBytes) +
                    " frames; differing frame:level" + differing + ")");
            const std::vector<char> segmentedAudio =
                readStream(segmented, true);
            const size_t samples =
                std::min(segmentedAudio.size(), fullAudio.size()) /
                sizeof(float);
            double worst =
                segmentedAudio.size() == fullAudio.size() ? 0.0 : 1.0;
            const auto* a =
                reinterpret_cast<const float*>(segmentedAudio.data());
            const auto* b = reinterpret_cast<const float*>(fullAudio.data());
            for (size_t index = 0; index < samples; ++index)
                worst = std::max(
                    worst, static_cast<double>(std::abs(a[index] - b[index])));
            Check(worst <= 1e-6,
                  "window: the sound of " + segmented +
                      " is the single pass's (worst " + std::to_string(worst) +
                      ", " + std::to_string(segmentedAudio.size()) +
                      " against " + std::to_string(fullAudio.size()) +
                      " bytes)");
            size_t leftovers = 0;
            for (const auto& entry :
                 std::filesystem::directory_iterator(directory))
                if (entry.path().filename().string().find("-segment-") !=
                    std::string::npos)
                    ++leftovers;
            Check(leftovers == 0,
                  "window: no segment is left beside the render (" +
                      std::to_string(leftovers) + ")");
        }
        // IA4 -- an inspection keeps a few frames of the window, chosen at
        // the graph's end: they must be those frames of the film, still.
        const std::vector<int64_t> keep = {41, 49, 50, 69};
        const std::string kept =
            managed ? "window-kept-managed" : "window-kept";
        if (!render(film, kept, kFirst, kEnd, keep)) continue;
        const std::vector<char> keptVideo = readStream(kept, false);
        Check(keptVideo.size() == frameBytes * keep.size(),
              "window: " + kept + " holds exactly the four frames kept (" +
                  std::to_string(keptVideo.size() / frameBytes) + ")");
        if (keptVideo.size() == frameBytes * keep.size() &&
            fullVideo.size() == frameBytes * 100) {
            int64_t differing = 0;
            for (size_t index = 0; index < keep.size(); ++index) {
                if (std::memcmp(keptVideo.data() + (frameBytes * index),
                                fullVideo.data() + (frameBytes * keep[index]),
                                frameBytes) != 0)
                    ++differing;
            }
            Check(differing == 0,
                  "window: the kept frames are the film's frames 41, 49, 50 "
                  "and 69, byte for byte (" +
                      std::to_string(differing) + " differ)");
        }
        // The two frames around a cut, the window's last one not among
        // them: the sound is still the whole window's. `-frames:v` used to
        // stop the file at the second kept frame and cut the sound there --
        // inspect_cut with two images refused a 4 s window of the reel with
        // 97 010 samples of the 192 000 it holds (25 September 2026).
        const std::string middle =
            managed ? "window-middle-managed" : "window-middle";
        if (!render(film, middle, kFirst, kEnd, {49, 50})) continue;
        const std::vector<char> middleVideo = readStream(middle, false);
        const std::vector<char> middleAudio = readStream(middle, true);
        const size_t windowAudioBytes = samplesPerFrame *
                                        static_cast<size_t>(kEnd - kFirst) *
                                        sizeof(float);
        Check(middleVideo.size() == frameBytes * 2 &&
                  middleAudio.size() == windowAudioBytes,
              "window: " + middle +
                  " keeps two frames and the window's whole sound (" +
                  std::to_string(middleAudio.size()) + " bytes of " +
                  std::to_string(windowAudioBytes) + ")");
    }
}

// R2 -- a long move. A zoom that changes size on every frame for six seconds
// spells 150 values, and the first spelling of the per-frame list, a flat
// sum, stopped parsing at 99 terms: a four-second push on a real S-Log3 rush
// refused to export, with or without colour management (measured on the
// LaCie rushes, 23 September 2026). Both paths export it now, and the last
// frame is the picture the engine evaluates for it.
void CheckLongAnimationExport(const std::filesystem::path& directory) {
    constexpr int kFrameWidth = 320;
    constexpr int kFrameHeight = 180;
    constexpr int kFrames = 150;
    const auto still = directory / "long-zoom-still.png";
    const auto rush = directory / "long-zoom-rush.mp4";
    Check(std::system((Quote(FFMPEG_EXECUTABLE) +
                       " -hide_banner -loglevel error -f lavfi -i "
                       "'testsrc2=size=640x360:rate=25:duration=0.04' "
                       "-frames:v 1 -y " +
                       Quote(still.string()))
                          .c_str()) == 0,
          "long zoom: the still renders");
    Check(
        std::system((Quote(FFMPEG_EXECUTABLE) +
                     " -hide_banner -loglevel error -loop 1 -framerate 25 "
                     "-i " +
                     Quote(still.string()) +
                     " -t 6.4 -vf \"lutyuv=y='64+val*160/255',format=yuv420p\" "
                     "-c:v libx264 -crf 0 -y " +
                     Quote(rush.string()))
                        .c_str()) == 0,
        "long zoom: the rush renders");
    Document document;
    document.sequence.width = kFrameWidth;
    document.sequence.height = kFrameHeight;
    document.sequence.frame_rate = {25, 1};
    const Ulid sourceId = "01K9E000000000000000000001";
    LibraryMedia media;
    media.id = sourceId;
    media.path = rush.string();
    media.filename = "long-zoom-rush.mp4";
    media.codec = "h264";
    media.orientation = "landscape";
    media.has_video = true;
    media.width = 640;
    media.height = 360;
    media.rate = {25, 1};
    media.duration = {160, 25};
    media.metadata_complete = true;
    document.library.push_back(media);
    document.sources.push_back({sourceId, rush.string(), {25, 1}, {160, 25}});
    DocumentClip clip;
    clip.id = "01K9E00000000000000000000C";
    clip.source_id = sourceId;
    clip.source_in = {0, 25};
    clip.duration = {kFrames, 25};
    clip.timeline_in = {0, 25};
    clip.include_audio = false;
    clip.framing.mode = FramingMode::Fill;
    ClipKeyframe wide;
    wide.id = "01K9E0000000000000000000K1";
    wide.time = {0, 25};
    wide.value = EffectParamValue{1, 1};
    ClipKeyframe close;
    close.id = "01K9E0000000000000000000K2";
    close.time = {kFrames - 1, 25};
    close.value = EffectParamValue{2, 1};
    ClipParameterAnimation zoom;
    zoom.parameter = ClipAnimatedParameter::FramingScale;
    zoom.keyframes = {wide, close};
    clip.animation = {zoom};
    document.sequence.tracks = {
        DocumentTrack{"01K9E0000000000000000000T0", "video", 0, {clip}}};
    std::string error;
    Check(document.Validate(error),
          "long zoom: the timeline validates: " + error);
    const size_t lumaBytes = static_cast<size_t>(kFrameWidth) * kFrameHeight;
    const size_t frameBytes = lumaBytes * 3 / 2;
    for (const bool managed : {false, true}) {
        const std::string name = managed ? "long-zoom-managed" : "long-zoom";
        Document exported = document;
        exported.color_management.enabled = managed;
        const auto output = directory / (name + ".mp4");
        ExportSettings settings = Exporter::H264Preset(output.string());
        settings.encoder = ExportEncoder::H264Software;
        settings.width = kFrameWidth;
        settings.height = kFrameHeight;
        settings.frame_rate = {25, 1};
        settings.ffmpeg_path = FFMPEG_EXECUTABLE;
        settings.video_bitrate = 50000000;
        ExportPlan plan;
        if (!Exporter::BuildPlan(exported,
                                 (directory / (name + ".json")).string(),
                                 settings, plan, error) ||
            !Exporter::Run(plan, {}, nullptr, error)) {
            Check(false, name + ": a six-second zoom exports: " + error);
            continue;
        }
        const auto raw = directory / (name + ".yuv");
        Check(std::system((Quote(FFMPEG_EXECUTABLE) +
                           " -hide_banner -loglevel error -i " +
                           Quote(output.string()) +
                           " -map 0:v:0 -an -sn -f rawvideo -pix_fmt yuv420p "
                           "-y " +
                           Quote(raw.string()))
                              .c_str()) == 0,
              name + ": the delivery decodes");
        std::ifstream input(raw, std::ios::binary);
        const std::vector<unsigned char> delivered(
            (std::istreambuf_iterator<char>(input)),
            std::istreambuf_iterator<char>());
        Check(
            delivered.size() == frameBytes * kFrames,
            name + ": the delivery is " + std::to_string(kFrames) + " frames");
        if (managed || delivered.size() != frameBytes * kFrames) continue;
        // The last frame against the engine's rectangle for it: the list
        // reached its end, and not the value of an earlier frame.
        ClipFramingGeometry geometry;
        EffectParamValue opacity{1, 1};
        Check(ResolveClipFramingAtFrame(
                  document, document.sequence.tracks[0].clips[0], kFrames - 1,
                  kFrameWidth, kFrameHeight, geometry, opacity, error),
              "long zoom: the last rectangle resolves: " + error);
        const auto reference = directory / "long-zoom-last.yuv";
        Check(std::system(
                  (Quote(FFMPEG_EXECUTABLE) +
                   " -hide_banner -loglevel error -i " + Quote(rush.string()) +
                   " -map 0:v:0 -frames:v 1 -vf \"scale=" +
                   std::to_string(geometry.full_width) + ':' +
                   std::to_string(geometry.full_height) +
                   ":flags=lanczos,crop=" + std::to_string(kFrameWidth) + ':' +
                   std::to_string(kFrameHeight) + ':' +
                   std::to_string(-geometry.full_x) + ':' +
                   std::to_string(-geometry.full_y) +
                   "\" -f rawvideo -pix_fmt yuv420p -y " +
                   Quote(reference.string()))
                      .c_str()) == 0,
              "long zoom: the expected last picture renders");
        std::ifstream expectedInput(reference, std::ios::binary);
        const std::vector<unsigned char> expected(
            (std::istreambuf_iterator<char>(expectedInput)),
            std::istreambuf_iterator<char>());
        if (expected.size() != frameBytes) continue;
        const unsigned char* last =
            delivered.data() + (frameBytes * static_cast<size_t>(kFrames - 1));
        double sum = 0.0;
        for (size_t offset = 0; offset < lumaBytes; ++offset)
            sum += std::abs(static_cast<int>(last[offset]) -
                            static_cast<int>(expected[offset]));
        const double match = sum / static_cast<double>(lumaBytes);
        Check(match < 2.5,
              "long zoom: the last frame is the engine's last "
              "rectangle (mean difference " +
                  std::to_string(match) + ')');
    }
}

// R2 -- an animated clip that enters by a cross dissolve. The export's chain
// for it starts at the dissolve's handle, before the clip's own first frame,
// and its per-frame lists used to be sampled from the clip's first frame and
// read from the handle's: every value landed `pre` frames early (found
// while bringing the same lists to the managed graph). The lists now cover
// the chain's frames, handle included, and this pins it: frames after the
// dissolve show the zoom the engine evaluates for THEM.
void CheckAnimatedClipAfterDissolve(const std::filesystem::path& directory) {
    constexpr int kSourceWidth = 1920;
    constexpr int kSourceHeight = 1080;
    constexpr int kFrameWidth = 1080;
    constexpr int kFrameHeight = 1920;
    constexpr int kFirstFrames = 10;
    constexpr int kSecondFrames = 12;
    constexpr int kFrames = kFirstFrames + kSecondFrames;
    const auto still = directory / "dissolve-zoom-still.png";
    const auto rush = directory / "dissolve-zoom-rush.mp4";
    const auto grey = directory / "dissolve-zoom-grey.mp4";
    Check(std::system((Quote(FFMPEG_EXECUTABLE) +
                       " -hide_banner -loglevel error -f lavfi -i "
                       "'testsrc2=size=1920x1080:rate=25:duration=0.04' "
                       "-frames:v 1 -y " +
                       Quote(still.string()))
                          .c_str()) == 0,
          "dissolve zoom: the still renders");
    Check(std::system((Quote(FFMPEG_EXECUTABLE) +
                       " -hide_banner -loglevel error -loop 1 -framerate 25 "
                       "-i " +
                       Quote(still.string()) +
                       " -t 1 -vf \"lutyuv=y='64+val*160/255',format=yuv420p\" "
                       "-c:v libx264 -crf 0 -y " +
                       Quote(rush.string()))
                          .c_str()) == 0,
          "dissolve zoom: the rush renders");
    Check(std::system((Quote(FFMPEG_EXECUTABLE) +
                       " -hide_banner -loglevel error -f lavfi -i "
                       "'color=c=0x404040:s=1920x1080:rate=25:duration=1' "
                       "-pix_fmt yuv420p -c:v libx264 -crf 0 -y " +
                       Quote(grey.string()))
                          .c_str()) == 0,
          "dissolve zoom: the grey rush renders");

    Document document;
    document.sequence.width = kFrameWidth;
    document.sequence.height = kFrameHeight;
    document.sequence.frame_rate = {25, 1};
    const auto addMedia = [&](const Ulid& id,
                              const std::filesystem::path& path) {
        LibraryMedia media;
        media.id = id;
        media.path = path.string();
        media.filename = path.filename().string();
        media.codec = "h264";
        media.orientation = "landscape";
        media.has_video = true;
        media.width = kSourceWidth;
        media.height = kSourceHeight;
        media.rate = {25, 1};
        media.duration = {25, 25};
        media.metadata_complete = true;
        document.library.push_back(media);
        document.sources.push_back({id, path.string(), {25, 1}, {25, 25}});
    };
    addMedia("01K9D000000000000000000001", grey);
    addMedia("01K9D000000000000000000002", rush);
    DocumentClip first;
    first.id = "01K9D00000000000000000000A";
    first.source_id = "01K9D000000000000000000001";
    first.source_in = {0, 25};
    first.duration = {kFirstFrames, 25};
    first.timeline_in = {0, 25};
    first.include_audio = false;
    first.framing.mode = FramingMode::Fill;
    DocumentClip second;
    second.id = "01K9D00000000000000000000B";
    second.source_id = "01K9D000000000000000000002";
    second.source_in = {4, 25};
    second.duration = {kSecondFrames, 25};
    second.timeline_in = {kFirstFrames, 25};
    second.include_audio = false;
    second.framing.mode = FramingMode::Fill;
    ClipKeyframe wide;
    wide.id = "01K9D0000000000000000000K1";
    wide.time = {0, 25};
    wide.value = EffectParamValue{1, 1};
    ClipKeyframe close;
    close.id = "01K9D0000000000000000000K2";
    close.time = {kSecondFrames, 25};
    close.value = EffectParamValue{3, 2};
    ClipParameterAnimation zoom;
    zoom.parameter = ClipAnimatedParameter::FramingScale;
    zoom.keyframes = {wide, close};
    second.animation = {zoom};
    document.sequence.tracks = {DocumentTrack{
        "01K9D0000000000000000000T0", "video", 0, {first, second}}};
    // Four frames centred on the cut: the chain of the second clip starts
    // two frames before its own first frame.
    DocumentTransition dissolve;
    dissolve.id = "01K9D0000000000000000000X1";
    dissolve.track_id = "01K9D0000000000000000000T0";
    dissolve.left_clip_id = first.id;
    dissolve.right_clip_id = second.id;
    dissolve.type = "cross_dissolve";
    dissolve.duration = {4, 25};
    dissolve.alignment = TransitionAlignment::Center;
    document.sequence.transitions = {dissolve};
    std::string error;
    Check(document.Validate(error),
          "dissolve zoom: the timeline validates: " + error);

    const auto output = directory / "dissolve-zoom.mp4";
    ExportSettings settings = Exporter::H264Preset(output.string());
    settings.encoder = ExportEncoder::H264Software;
    settings.width = kFrameWidth;
    settings.height = kFrameHeight;
    settings.frame_rate = {25, 1};
    settings.ffmpeg_path = FFMPEG_EXECUTABLE;
    settings.video_bitrate = 200000000;
    ExportPlan plan;
    if (!Exporter::BuildPlan(document,
                             (directory / "dissolve-zoom.json").string(),
                             settings, plan, error) ||
        !Exporter::Run(plan, {}, nullptr, error)) {
        Check(false, "dissolve zoom: the film exports: " + error);
        return;
    }
    const size_t lumaBytes = static_cast<size_t>(kFrameWidth) * kFrameHeight;
    const size_t frameBytes = lumaBytes * 3 / 2;
    const auto raw = directory / "dissolve-zoom.yuv";
    Check(std::system((Quote(FFMPEG_EXECUTABLE) +
                       " -hide_banner -loglevel error -i " +
                       Quote(output.string()) +
                       " -map 0:v:0 -an -sn -f rawvideo -pix_fmt yuv420p -y " +
                       Quote(raw.string()))
                          .c_str()) == 0,
          "dissolve zoom: the delivery decodes");
    std::ifstream input(raw, std::ios::binary);
    const std::vector<unsigned char> delivered(
        (std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>());
    Check(delivered.size() == frameBytes * kFrames,
          "dissolve zoom: the delivery is " + std::to_string(kFrames) +
              " frames");
    if (delivered.size() != frameBytes * kFrames) return;
    // Local frames 2 (the first past the dissolve), 6 and 11 of the second
    // clip, each against the picture its own rectangle gives, and against
    // the rectangle two frames later -- where the old lists put it.
    for (const int64_t local : {int64_t{2}, int64_t{6}, int64_t{9}}) {
        const auto expectedAt = [&](int64_t frame,
                                    std::vector<unsigned char>& pixels) {
            ClipFramingGeometry geometry;
            EffectParamValue opacity{1, 1};
            std::string sampleError;
            if (!ResolveClipFramingAtFrame(
                    document, document.sequence.tracks[0].clips[1], frame,
                    kFrameWidth, kFrameHeight, geometry, opacity, sampleError))
                return false;
            const auto reference = directory / ("dissolve-zoom-ref-" +
                                                std::to_string(frame) + ".yuv");
            const std::string render =
                Quote(FFMPEG_EXECUTABLE) + " -hide_banner -loglevel error -i " +
                Quote(rush.string()) + " -map 0:v:0 -frames:v 1 -vf \"scale=" +
                std::to_string(geometry.full_width) + ':' +
                std::to_string(geometry.full_height) +
                ":flags=lanczos,crop=" + std::to_string(kFrameWidth) + ':' +
                std::to_string(kFrameHeight) + ':' +
                std::to_string(-geometry.full_x) + ':' +
                std::to_string(-geometry.full_y) +
                "\" -f rawvideo -pix_fmt yuv420p -y " +
                Quote(reference.string());
            if (std::system(render.c_str()) != 0) return false;
            std::ifstream in(reference, std::ios::binary);
            pixels.assign(std::istreambuf_iterator<char>(in),
                          std::istreambuf_iterator<char>());
            return pixels.size() == frameBytes;
        };
        std::vector<unsigned char> expected;
        std::vector<unsigned char> early;
        Check(expectedAt(local, expected) && expectedAt(local + 2, early),
              "dissolve zoom: the expected pictures render");
        if (expected.size() != frameBytes || early.size() != frameBytes)
            continue;
        const size_t at = static_cast<size_t>(kFirstFrames + local);
        const auto meanDifference =
            [&](const std::vector<unsigned char>& left) {
                const unsigned char* data =
                    delivered.data() + (frameBytes * at);
                double sum = 0.0;
                for (size_t offset = 0; offset < lumaBytes; ++offset)
                    sum += std::abs(static_cast<int>(data[offset]) -
                                    static_cast<int>(left[offset]));
                return sum / static_cast<double>(lumaBytes);
            };
        const double match = meanDifference(expected);
        const double shifted = meanDifference(early);
        Check(match < 2.0 && match < shifted,
              "dissolve zoom: local frame " + std::to_string(local) +
                  " shows its own zoom (mean difference " +
                  std::to_string(match) + ", against " +
                  std::to_string(shifted) + " for the zoom two frames on)");
    }
}

// R2 -- an animated opacity is an alpha plane that moves, and the export
// generates it. A flat clip over a black base at a falling opacity is what
// the two cut-down versions of the film used for their flashes, so the
// measurement is the delivered luma of the frames of the fade.
void CheckAnimatedOpacityExport(const std::filesystem::path& directory) {
    constexpr int kFrameWidth = 320;
    constexpr int kFrameHeight = 180;
    constexpr int kFrames = 5;
    const auto still = directory / "flash-still.png";
    const auto rush = directory / "flash-rush.mp4";
    Check(std::system((Quote(FFMPEG_EXECUTABLE) +
                       " -hide_banner -loglevel error -f lavfi -i "
                       "'color=c=white:s=320x180:rate=25:duration=0.04' "
                       "-frames:v 1 -y " +
                       Quote(still.string()))
                          .c_str()) == 0,
          "flash: the still renders");
    Check(std::system((Quote(FFMPEG_EXECUTABLE) +
                       " -hide_banner -loglevel error -loop 1 -framerate 25 "
                       "-i " +
                       Quote(still.string()) +
                       " -t 0.4 -pix_fmt yuv420p -c:v libx264 -crf 0 -y " +
                       Quote(rush.string()))
                          .c_str()) == 0,
          "flash: the rush renders");
    Document document;
    document.sequence.width = kFrameWidth;
    document.sequence.height = kFrameHeight;
    document.sequence.frame_rate = {25, 1};
    const Ulid sourceId = "01K9B000000000000000000001";
    LibraryMedia media;
    media.id = sourceId;
    media.path = rush.string();
    media.filename = "flash-rush.mp4";
    media.codec = "h264";
    media.orientation = "landscape";
    media.has_video = true;
    media.width = kFrameWidth;
    media.height = kFrameHeight;
    media.rate = {25, 1};
    media.duration = {10, 25};
    media.metadata_complete = true;
    document.library.push_back(media);
    document.sources.push_back({sourceId, rush.string(), {25, 1}, {10, 25}});
    DocumentClip clip;
    clip.id = "01K9B00000000000000000000C";
    clip.source_id = sourceId;
    clip.source_in = {0, 25};
    clip.duration = {kFrames, 25};
    clip.timeline_in = {0, 25};
    clip.include_audio = false;
    ClipKeyframe opaque;
    opaque.id = "01K9B0000000000000000000K1";
    opaque.time = {0, 25};
    opaque.value = EffectParamValue{1, 1};
    ClipKeyframe gone;
    gone.id = "01K9B0000000000000000000K2";
    gone.time = {4, 25};
    gone.value = EffectParamValue{0, 1};
    ClipParameterAnimation fade;
    fade.parameter = ClipAnimatedParameter::Opacity;
    fade.keyframes = {opaque, gone};
    clip.animation = {fade};
    document.sequence.tracks = {
        DocumentTrack{"01K9B0000000000000000000T0", "video", 0, {clip}}};
    std::string validationError;
    Check(document.Validate(validationError),
          "flash: the timeline validates: " + validationError);

    const auto output = directory / "flash.mp4";
    ExportSettings settings = Exporter::H264Preset(output.string());
    settings.encoder = ExportEncoder::H264Software;
    settings.width = kFrameWidth;
    settings.height = kFrameHeight;
    settings.frame_rate = {25, 1};
    settings.ffmpeg_path = FFMPEG_EXECUTABLE;
    settings.video_bitrate = 100000000;
    ExportPlan plan;
    std::string error;
    if (!Exporter::BuildPlan(document, (directory / "flash.json").string(),
                             settings, plan, error) ||
        !Exporter::Run(plan, {}, nullptr, error)) {
        Check(false, "flash: the fade exports: " + error);
        return;
    }
    const auto raw = directory / "flash.yuv";
    Check(std::system((Quote(FFMPEG_EXECUTABLE) +
                       " -hide_banner -loglevel error -i " +
                       Quote(output.string()) +
                       " -map 0:v:0 -an -sn -f rawvideo -pix_fmt yuv420p -y " +
                       Quote(raw.string()))
                          .c_str()) == 0,
          "flash: the delivery decodes");
    std::ifstream input(raw, std::ios::binary);
    const std::vector<unsigned char> pixels(
        (std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>());
    const size_t lumaBytes = static_cast<size_t>(kFrameWidth) * kFrameHeight;
    const size_t frameBytes = lumaBytes * 3 / 2;
    Check(pixels.size() == frameBytes * kFrames,
          "flash: the delivery is five frames");
    if (pixels.size() != frameBytes * kFrames) return;
    double previous = 1e9;
    for (int frame = 0; frame < kFrames; ++frame) {
        const unsigned char* data =
            pixels.data() + (frameBytes * static_cast<size_t>(frame));
        double sum = 0.0;
        for (size_t offset = 0; offset < lumaBytes; ++offset)
            sum += data[offset];
        const double mean = sum / static_cast<double>(lumaBytes);
        Check(mean < previous - 10.0,
              "flash: frame " + std::to_string(frame) +
                  " is darker than the one before it (" + std::to_string(mean) +
                  ')');
        previous = mean;
    }
    const unsigned char* lastFrame =
        pixels.data() + (frameBytes * static_cast<size_t>(kFrames - 1));
    double lastSum = 0.0;
    for (size_t offset = 0; offset < lumaBytes; ++offset)
        lastSum += lastFrame[offset];
    Check((lastSum / static_cast<double>(lumaBytes)) < 24.0,
          "flash: the last frame is gone, as the last key says");

    // R2 -- the same fade under colour management. The managed composite
    // used to refuse it (its `pad` cannot move a layer frame by frame); the
    // opacity now reaches its float alpha frame by frame through `sendcmd`,
    // and the mix happens in linear light as on the monitor, so the ramp is
    // not the unmanaged one -- what must hold is that every frame is darker
    // than the one before and that the last is gone. The values themselves
    // are held to the monitor by tests/monitor_export_parity_tests.mm.
    {
        Document managed = document;
        managed.color_management.enabled = true;
        const auto managedOutput = directory / "flash-managed.mp4";
        ExportSettings managedSettings = settings;
        managedSettings.output_path = managedOutput.string();
        ExportPlan managedPlan;
        std::string managedError;
        const bool exported =
            Exporter::BuildPlan(managed,
                                (directory / "flash-managed.json").string(),
                                managedSettings, managedPlan, managedError) &&
            Exporter::Run(managedPlan, {}, nullptr, managedError);
        Check(exported, "flash: the fade exports under colour management: " +
                            managedError);
        const auto managedRaw = directory / "flash-managed.yuv";
        const bool decoded =
            exported &&
            std::system(
                (Quote(FFMPEG_EXECUTABLE) +
                 " -hide_banner -loglevel error -i " +
                 Quote(managedOutput.string()) +
                 " -map 0:v:0 -an -sn -f rawvideo -pix_fmt yuv420p -y " +
                 Quote(managedRaw.string()))
                    .c_str()) == 0;
        std::ifstream managedInput(managedRaw, std::ios::binary);
        const std::vector<unsigned char> managedPixels(
            (std::istreambuf_iterator<char>(managedInput)),
            std::istreambuf_iterator<char>());
        Check(decoded && managedPixels.size() == frameBytes * kFrames,
              "flash: the managed delivery is five frames");
        if (decoded && managedPixels.size() == frameBytes * kFrames) {
            double managedPrevious = 1e9;
            double managedMean = 0.0;
            for (int frame = 0; frame < kFrames; ++frame) {
                const unsigned char* data =
                    managedPixels.data() +
                    (frameBytes * static_cast<size_t>(frame));
                double sum = 0.0;
                for (size_t offset = 0; offset < lumaBytes; ++offset)
                    sum += data[offset];
                managedMean = sum / static_cast<double>(lumaBytes);
                Check(managedMean < managedPrevious - 5.0,
                      "flash: managed frame " + std::to_string(frame) +
                          " is darker than the one before it (" +
                          std::to_string(managedMean) + ')');
                managedPrevious = managedMean;
            }
            Check(managedMean < 24.0,
                  "flash: the managed last frame is gone too (" +
                      std::to_string(managedMean) + ')');
        }
    }

    // VQA2 -- docs/roadmap/QA-AGENTIQUE-2026-09.md. The same fade on a clip
    // whose zoom is animated too, so the picture changes size every frame.
    // The opacity used to go on through `alphamerge`, which crashed FFmpeg
    // 8.1 on exactly this clip six runs in six while either animation alone
    // exported. Three runs, because the crash was a memory fault, and at
    // 1920x1080: on a small frame the fault stayed silent.
    ClipKeyframe wide;
    wide.id = "01K9B0000000000000000000K3";
    wide.time = {0, 25};
    wide.value = EffectParamValue{1, 1};
    ClipKeyframe close;
    close.id = "01K9B0000000000000000000K4";
    close.time = {4, 25};
    close.value = EffectParamValue{23, 20};
    ClipParameterAnimation zoom;
    zoom.parameter = ClipAnimatedParameter::FramingScale;
    zoom.keyframes = {wide, close};
    constexpr int kZoomedWidth = 1920;
    constexpr int kZoomedHeight = 1080;
    Document zoomed = document;
    zoomed.sequence.width = kZoomedWidth;
    zoomed.sequence.height = kZoomedHeight;
    zoomed.sequence.tracks.front().clips.front().animation = {fade, zoom};
    Check(zoomed.Validate(validationError),
          "flash + zoom: the timeline validates: " + validationError);
    for (int run = 0; run < 3; ++run) {
        const auto zoomedOutput =
            directory / ("flash-zoom-" + std::to_string(run) + ".mp4");
        ExportSettings zoomedSettings = settings;
        zoomedSettings.output_path = zoomedOutput.string();
        zoomedSettings.width = kZoomedWidth;
        zoomedSettings.height = kZoomedHeight;
        ExportPlan zoomedPlan;
        std::string zoomedError;
        const bool exported =
            Exporter::BuildPlan(zoomed,
                                (directory / "flash-zoom.json").string(),
                                zoomedSettings, zoomedPlan, zoomedError) &&
            Exporter::Run(zoomedPlan, {}, nullptr, zoomedError);
        Check(exported, "flash + zoom: run " + std::to_string(run) +
                            " exports: " + zoomedError);
        if (!exported) return;
    }
    const auto zoomedRaw = directory / "flash-zoom.yuv";
    Check(std::system((Quote(FFMPEG_EXECUTABLE) +
                       " -hide_banner -loglevel error -i " +
                       Quote((directory / "flash-zoom-0.mp4").string()) +
                       " -map 0:v:0 -an -sn -f rawvideo -pix_fmt yuv420p -y " +
                       Quote(zoomedRaw.string()))
                          .c_str()) == 0,
          "flash + zoom: the delivery decodes");
    std::ifstream zoomedInput(zoomedRaw, std::ios::binary);
    const std::vector<unsigned char> zoomedPixels(
        (std::istreambuf_iterator<char>(zoomedInput)),
        std::istreambuf_iterator<char>());
    const size_t zoomedLumaBytes =
        static_cast<size_t>(kZoomedWidth) * kZoomedHeight;
    const size_t zoomedFrameBytes = zoomedLumaBytes * 3 / 2;
    Check(zoomedPixels.size() == zoomedFrameBytes * kFrames,
          "flash + zoom: the delivery is five frames");
    if (zoomedPixels.size() != zoomedFrameBytes * kFrames) return;
    double zoomedPrevious = 1e9;
    for (int frame = 0; frame < kFrames; ++frame) {
        const unsigned char* data =
            zoomedPixels.data() +
            (zoomedFrameBytes * static_cast<size_t>(frame));
        double sum = 0.0;
        for (size_t offset = 0; offset < zoomedLumaBytes; ++offset)
            sum += data[offset];
        const double mean = sum / static_cast<double>(zoomedLumaBytes);
        Check(mean < zoomedPrevious - 10.0,
              "flash + zoom: frame " + std::to_string(frame) +
                  " is darker than the one before it (" + std::to_string(mean) +
                  ')');
        zoomedPrevious = mean;
    }
}

// R5 -- docs/roadmap/REEL-2026-09.md, the acceptance criterion and the one
// added on 19 September. A film that opens on black, dips through white at
// a cut and closes on black, exported for real and read back frame by
// frame: the delivered luma at mid-fade has to be the blend the engine's
// own alpha names, and not a curve FFmpeg happens to draw.
//
// The rush is flat grey on purpose. A dip blends every pixel towards the
// colour, so a flat picture makes the delivered mean the blend itself,
// with nothing else moving underneath it -- the fully veiled frames give
// the two ends of the blend, measured rather than assumed, and every other
// frame is checked against TransitionRender.h's fraction.
void CheckColorTransitionExport(const std::filesystem::path& directory) {
    constexpr int kFrameWidth = 320;
    constexpr int kFrameHeight = 180;
    constexpr int kFrames = 24;
    const auto rush = directory / "dip-rush.mp4";
    Check(std::system((Quote(FFMPEG_EXECUTABLE) +
                       " -hide_banner -loglevel error -f lavfi -i "
                       "'color=c=0x808080:s=320x180:rate=25:duration=2' "
                       "-pix_fmt yuv420p -c:v libx264 -crf 0 -y " +
                       Quote(rush.string()))
                          .c_str()) == 0,
          "dip: the grey rush renders");
    Document document;
    document.sequence.width = kFrameWidth;
    document.sequence.height = kFrameHeight;
    document.sequence.frame_rate = {25, 1};
    const Ulid sourceId = "01K9C000000000000000000001";
    LibraryMedia media;
    media.id = sourceId;
    media.path = rush.string();
    media.filename = "dip-rush.mp4";
    media.codec = "h264";
    media.orientation = "landscape";
    media.has_video = true;
    media.width = kFrameWidth;
    media.height = kFrameHeight;
    media.rate = {25, 1};
    media.duration = {50, 25};
    media.metadata_complete = true;
    document.library.push_back(media);
    document.sources.push_back({sourceId, rush.string(), {25, 1}, {50, 25}});
    DocumentClip first;
    first.id = "01K9C00000000000000000000A";
    first.source_id = sourceId;
    first.source_in = {0, 25};
    first.duration = {12, 25};
    first.timeline_in = {0, 25};
    first.include_audio = false;
    DocumentClip second = first;
    second.id = "01K9C00000000000000000000B";
    second.source_in = {12, 25};
    second.timeline_in = {12, 25};
    document.sequence.tracks = {DocumentTrack{
        "01K9C0000000000000000000T0", "video", 0, {first, second}}};

    // The film measured on 19 September, in miniature: it opens on black,
    // and it closes on black. Between the two shots, a white dip -- the
    // "pixels read in the middle of a fade to white" the ticket asks for,
    // and the one frame of the whole delivery that is purely the colour.
    DocumentTransition opening;
    opening.id = "01K9C0000000000000000000X1";
    opening.track_id = document.sequence.tracks[0].id;
    opening.right_clip_id = first.id;
    opening.type = "dip_to_color";
    opening.duration = {6, 25};
    opening.alignment = TransitionAlignment::StartAtCut;
    DocumentTransition middle;
    middle.id = "01K9C0000000000000000000X2";
    middle.track_id = document.sequence.tracks[0].id;
    middle.left_clip_id = first.id;
    middle.right_clip_id = second.id;
    middle.type = "dip_to_color";
    middle.color = "#ffffff";
    middle.duration = {6, 25};
    middle.alignment = TransitionAlignment::Center;
    DocumentTransition closing;
    closing.id = "01K9C0000000000000000000X3";
    closing.track_id = document.sequence.tracks[0].id;
    closing.left_clip_id = second.id;
    closing.type = "dip_to_color";
    closing.duration = {6, 25};
    closing.alignment = TransitionAlignment::EndAtCut;
    document.sequence.transitions = {opening, middle, closing};
    std::string validationError;
    Check(document.Validate(validationError),
          "dip: the timeline validates: " + validationError);

    // A dip veils the track; a clip composited at less than full strength
    // would take the veil under it instead of over it. Refused by name.
    {
        Document translucent = document;
        translucent.sequence.tracks[0].clips[0].opacity =
            EffectParamValue{1, 2};
        ExportSettings refusedSettings =
            Exporter::H264Preset((directory / "dip-translucent.mp4").string());
        refusedSettings.encoder = ExportEncoder::H264Software;
        refusedSettings.width = kFrameWidth;
        refusedSettings.height = kFrameHeight;
        refusedSettings.frame_rate = {25, 1};
        refusedSettings.ffmpeg_path = FFMPEG_EXECUTABLE;
        ExportPlan refusedPlan;
        std::string refusedError;
        Check(
            !Exporter::BuildPlan(translucent,
                                 (directory / "dip-translucent.json").string(),
                                 refusedSettings, refusedPlan, refusedError) &&
                refusedError.find("whose opacity is not 1/1") !=
                    std::string::npos,
            "dip: a veil over a translucent clip is refused: " + refusedError);
    }

    const auto output = directory / "dip.mp4";
    const auto projectPath = directory / "dip.json";
    ExportSettings settings = Exporter::H264Preset(output.string());
    settings.encoder = ExportEncoder::H264Software;
    settings.width = kFrameWidth;
    settings.height = kFrameHeight;
    settings.frame_rate = {25, 1};
    settings.ffmpeg_path = FFMPEG_EXECUTABLE;
    settings.video_bitrate = 100000000;
    ExportPlan plan;
    std::string error;
    if (!Exporter::BuildPlan(document, projectPath.string(), settings, plan,
                             error) ||
        !Exporter::Run(plan, {}, nullptr, error)) {
        Check(false, "dip: the film exports: " + error);
        return;
    }
    const auto raw = directory / "dip.yuv";
    Check(std::system((Quote(FFMPEG_EXECUTABLE) +
                       " -hide_banner -loglevel error -i " +
                       Quote(output.string()) +
                       " -map 0:v:0 -an -sn -f rawvideo -pix_fmt yuv420p -y " +
                       Quote(raw.string()))
                          .c_str()) == 0,
          "dip: the delivery decodes");
    std::ifstream input(raw, std::ios::binary);
    const std::vector<unsigned char> pixels(
        (std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>());
    const size_t lumaBytes = static_cast<size_t>(kFrameWidth) * kFrameHeight;
    const size_t frameBytes = lumaBytes * 3 / 2;
    Check(pixels.size() == frameBytes * kFrames,
          "dip: the delivery is 24 frames");
    if (pixels.size() != frameBytes * kFrames) return;
    const auto luma = [&](int frame) {
        const unsigned char* data =
            pixels.data() + (frameBytes * static_cast<size_t>(frame));
        double sum = 0.0;
        for (size_t offset = 0; offset < lumaBytes; ++offset)
            sum += data[offset];
        return sum / static_cast<double>(lumaBytes);
    };
    // The three references, measured and not assumed: the shot itself, the
    // black of frame 0 (the opening's alpha is exactly 1 there) and the
    // white of frame 12 (the dip's peak sits on the cut).
    const double picture = luma(8);
    const double black = luma(0);
    const double white = luma(12);
    Check(black < picture - 60.0 && white > picture + 60.0,
          "dip: the two fully veiled frames are the colours they name (" +
              std::to_string(black) + ", " + std::to_string(picture) + ", " +
              std::to_string(white) + ')');
    // VQA2 -- docs/roadmap/QA-AGENTIQUE-2026-09.md. And they are the video
    // levels of those colours, not merely darker and lighter: the references
    // above are measured from the file, so a veil that missed video black
    // would move them and the ramp together and pass unseen.
    Check(std::abs(black - 16.0) <= 2.0 && std::abs(white - 235.0) <= 2.0,
          "dip: black and white are delivered at video levels 16 and 235 (" +
              std::to_string(black) + ", " + std::to_string(white) + ')');
    // Every other frame the three dips cover, against the engine's own
    // fraction. Guard rail 1: if the export drew any other curve, or
    // shifted it by a frame, this is where it shows.
    const Timeline timeline(document);
    for (int frame = 0; frame < kFrames; ++frame) {
        const std::optional<ResolvedColorVeil> veil =
            timeline.ResolveTrackColorVeil(document.sequence.tracks[0].id,
                                           RationalTime{frame, 25});
        const double alpha = veil ? TransitionAlphaFloat(veil->alpha) : 0.0;
        const double target = veil && veil->color.red != 0 ? white : black;
        const double expected = picture + (target - picture) * alpha;
        Check(std::abs(luma(frame) - expected) <= 3.0,
              "dip: frame " + std::to_string(frame) + " is " +
                  std::to_string(luma(frame)) + ", the engine says " +
                  std::to_string(expected));
    }

    // R5 -- the same film under colour management. The dips are laid in
    // the SDR view (Export.cc, AppendManagedLayer), which on a Rec.709
    // delivery is the delivered signal: so the managed file must draw the
    // very ramps the unmanaged one does, in display values, and its black
    // and white must be video black and white. Laid in the camera's
    // encoding, as the first attempt did, a white dip on an S-Log3 rush was
    // measured 0.26 away from the monitor; the monitor side of this is
    // tests/monitor_export_parity_tests.mm.
    {
        Document managed = document;
        managed.color_management.enabled = true;
        const auto managedOutput = directory / "dip-managed.mp4";
        ExportSettings managedSettings = settings;
        managedSettings.output_path = managedOutput.string();
        ExportPlan managedPlan;
        std::string managedError;
        const bool exported =
            Exporter::BuildPlan(managed,
                                (directory / "dip-managed.json").string(),
                                managedSettings, managedPlan, managedError) &&
            Exporter::Run(managedPlan, {}, nullptr, managedError);
        Check(exported,
              "dip: the film exports under colour management: " + managedError);
        const auto managedRaw = directory / "dip-managed.yuv";
        const bool decoded =
            exported &&
            std::system(
                (Quote(FFMPEG_EXECUTABLE) +
                 " -hide_banner -loglevel error -i " +
                 Quote(managedOutput.string()) +
                 " -map 0:v:0 -an -sn -f rawvideo -pix_fmt yuv420p -y " +
                 Quote(managedRaw.string()))
                    .c_str()) == 0;
        std::ifstream managedInput(managedRaw, std::ios::binary);
        const std::vector<unsigned char> managedPixels(
            (std::istreambuf_iterator<char>(managedInput)),
            std::istreambuf_iterator<char>());
        Check(decoded && managedPixels.size() == frameBytes * kFrames,
              "dip: the managed delivery is 24 frames");
        if (decoded && managedPixels.size() == frameBytes * kFrames) {
            const auto managedLuma = [&](int frame) {
                const unsigned char* data =
                    managedPixels.data() +
                    (frameBytes * static_cast<size_t>(frame));
                double sum = 0.0;
                for (size_t offset = 0; offset < lumaBytes; ++offset)
                    sum += data[offset];
                return sum / static_cast<double>(lumaBytes);
            };
            const double managedPicture = managedLuma(8);
            const double managedBlack = managedLuma(0);
            const double managedWhite = managedLuma(12);
            Check(std::abs(managedBlack - 16.0) <= 2.0 &&
                      std::abs(managedWhite - 235.0) <= 2.0,
                  "dip: under colour management too, black and white are "
                  "delivered at video levels 16 and 235 (" +
                      std::to_string(managedBlack) + ", " +
                      std::to_string(managedWhite) + ')');
            for (int frame = 0; frame < kFrames; ++frame) {
                const std::optional<ResolvedColorVeil> veil =
                    timeline.ResolveTrackColorVeil(
                        document.sequence.tracks[0].id,
                        RationalTime{frame, 25});
                const double alpha =
                    veil ? TransitionAlphaFloat(veil->alpha) : 0.0;
                const double target =
                    veil && veil->color.red != 0 ? managedWhite : managedBlack;
                const double expected =
                    managedPicture + (target - managedPicture) * alpha;
                Check(std::abs(managedLuma(frame) - expected) <= 3.0,
                      "dip: managed frame " + std::to_string(frame) + " is " +
                          std::to_string(managedLuma(frame)) +
                          ", the engine says " + std::to_string(expected));
            }
        }
    }

    // verify_export -- the criterion's other half. A frame the veil covers
    // entirely is the colour and nothing else, so a black one is checked as
    // black; anywhere else on the ramp the delivered picture is a blend no
    // source thumbnail matches, and it is counted invérifiable rather than
    // judged wrong.
    Check(document.Save(projectPath.string(), error),
          "dip: the project saves: " + error);
    ExportVerificationSettings verification;
    verification.ffmpeg_path = FFMPEG_EXECUTABLE;
    ExportVerificationReport report;
    Check(VerifyExport(document, directory, output.string(), verification,
                       report, error),
          "dip: the delivery verifies: " + error);
    // Five per dip. Each one covers six frames, and the sixth is checked:
    // the opening's first frame is fully black and reads as black, while
    // the white dip and the closing each open on an alpha of exactly zero,
    // which is the picture itself. Frame 12, the white peak, is counted
    // invérifiable and not black -- the colour is not black.
    Check(report.unverifiable_frames == 15,
          "dip: 15 frames of ramp counted invérifiable, got " +
              std::to_string(report.unverifiable_frames));
    Check(report.checked_frames == kFrames - 15,
          "dip: every other frame is checked, got " +
              std::to_string(report.checked_frames));
    Check(report.divergent_frames == 0,
          "dip: and none of them diverges, got " +
              std::to_string(report.divergent_frames));
    // PRD38 -- and the report does not call that exact: 9 frames of 24
    // were compared.
    const std::string dipJson = SerializeExportVerification(report);
    Check(dipJson.find("\"exact\":false,\"verified_fraction\":0.375,") !=
                  std::string::npos &&
              dipJson.find("15 of the 24 expected frames were not compared") !=
                  std::string::npos,
          "dip: the ramp's frames keep the report from reading exact: " +
              dipJson);
    // VQA2 -- docs/roadmap/QA-AGENTIQUE-2026-09.md. Rec.709-A, as DaVinci
    // Resolve writes it (measured on two of its deliveries): colr 1-2-1 and
    // a gama atom of 2.4, in a .mov. An .mp4 has no gama atom and is shown as
    // 1-1-1, so it is refused rather than tagged in vain.
    const auto colourAtoms = [&](const std::filesystem::path& file) {
        const auto trace = directory / "atoms.txt";
        // The demuxer's trace names every colour atom it reads.
        std::system((Quote(FFMPEG_EXECUTABLE) + " -hide_banner -v trace -i " +
                     Quote(file.string()) + " -frames:v 1 -f null - > " +
                     Quote(trace.string()) + " 2>&1")
                        .c_str());
        std::ifstream in(trace);
        std::string text((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
        return text;
    };
    {
        ExportSettings tagged = settings;
        tagged.sdr_tag = ExportSdrTag::Rec709A;
        tagged.output_path = (directory / "dip-709a.mp4").string();
        ExportPlan taggedPlan;
        std::string taggedError;
        Check(!Exporter::BuildPlan(document, projectPath.string(), tagged,
                                   taggedPlan, taggedError) &&
                  taggedError.find("Rec.709-A needs a QuickTime .mov") !=
                      std::string::npos,
              "rec709a: an .mp4 is refused by name: " + taggedError);
        tagged.output_path = (directory / "dip-709a.mov").string();
        const bool exported =
            Exporter::BuildPlan(document, projectPath.string(), tagged,
                                taggedPlan, taggedError) &&
            Exporter::Run(taggedPlan, {}, nullptr, taggedError);
        Check(exported, "rec709a: the .mov exports: " + taggedError);
        const std::string atoms = colourAtoms(tagged.output_path);
        Check(atoms.find("nclc: pri 1 trc 2 matrix 1") != std::string::npos,
              "rec709a: colr is 1-2-1, as Resolve writes it");
        Check(atoms.find("type:'gama'") != std::string::npos,
              "rec709a: a gama atom is written");
        ExportSettings plain = settings;
        plain.output_path = (directory / "dip-709.mov").string();
        ExportPlan plainPlan;
        const bool plainExported =
            Exporter::BuildPlan(document, projectPath.string(), plain,
                                plainPlan, taggedError) &&
            Exporter::Run(plainPlan, {}, nullptr, taggedError);
        const std::string plainAtoms = colourAtoms(plain.output_path);
        Check(plainExported &&
                  plainAtoms.find("type:'gama'") == std::string::npos &&
                  plainAtoms.find("trc 1") != std::string::npos,
              "rec709a: the default stays Rec.709, 1-1-1 without gama");
    }
}

// R1 -- guard rail 1. verify_export resolves the expected picture through
// Timeline and ClipFraming.h, so it has to find a framed export exact --
// and has to see a framing the file does not have as wrong. That is the
// test that would catch the export and the engine parting over the same
// rectangle.
void CheckFramingVerification(const std::filesystem::path& directory) {
    const auto source = directory / "framing-testsrc.mp4";
    const std::string generate =
        Quote(FFMPEG_EXECUTABLE) +
        " -hide_banner -loglevel error -f lavfi -i "
        "'testsrc2=size=640x360:rate=25:duration=1' -c:v libx264 -crf 12 "
        "-pix_fmt yuv420p -y " +
        Quote(source.string());
    Check(std::system(generate.c_str()) == 0,
          "framing: verification source written");

    Document document;
    document.sequence.width = 360;
    document.sequence.height = 640;
    document.sequence.frame_rate = {25, 1};
    const Ulid sourceId = "01K6F7W1000000000000000002";
    LibraryMedia media;
    media.id = sourceId;
    media.path = source.string();
    media.filename = "framing-testsrc.mp4";
    media.codec = "h264";
    media.orientation = "landscape";
    media.has_video = true;
    media.width = 640;
    media.height = 360;
    media.rate = {25, 1};
    media.duration = {25, 25};
    media.metadata_complete = true;
    document.library.push_back(media);
    document.sources.push_back({sourceId, source.string(), {25, 1}, {25, 25}});
    DocumentClip clip;
    clip.id = "01K6F7W1000000000000000021";
    clip.source_id = sourceId;
    clip.source_in = {0, 25};
    clip.duration = {10, 25};
    clip.timeline_in = {0, 25};
    clip.include_audio = false;
    clip.framing.mode = FramingMode::Fill;
    clip.framing.scale = {5, 4};
    document.sequence.tracks = {
        {"01K6F7W1000000000000000020", "video", 0, {clip}}};
    std::string validationError;
    Check(document.Validate(validationError),
          "framing: the verified timeline validates: " + validationError);

    const auto output = directory / "framing-verify.mp4";
    ExportSettings settings = Exporter::H264Preset(output.string());
    settings.encoder = ExportEncoder::H264Software;
    settings.width = 360;
    settings.height = 640;
    settings.frame_rate = {25, 1};
    settings.ffmpeg_path = FFMPEG_EXECUTABLE;
    ExportPlan plan;
    std::string error;
    const bool rendered =
        Exporter::BuildPlan(document,
                            (directory / "framing-verify.json").string(),
                            settings, plan, error) &&
        Exporter::Run(plan, {}, nullptr, error);
    Check(rendered, "framing: the verified timeline exports: " + error);
    if (!rendered) return;

    ExportVerificationSettings verification;
    verification.ffmpeg_path = FFMPEG_EXECUTABLE;
    ExportVerificationReport report;
    Check(VerifyExport(document, directory, output.string(), verification,
                       report, error) &&
              report.expected_frames == 10 && report.divergent_frames == 0,
          "framing: verify_export finds the framed export exact: " + error +
              ' ' + SerializeExportVerification(report));

    // And a document that claims another framing must not pass: the
    // delivered picture is a crop of the source, and a fitted expectation
    // is a different picture entirely.
    Document wrong = document;
    wrong.sequence.tracks[0].clips[0].framing = ClipFraming{};
    Check(VerifyExport(wrong, directory, output.string(), verification, report,
                       error) &&
              report.divergent_frames > 0,
          "framing: verify_export sees a framing the file does not have: " +
              error + ' ' + SerializeExportVerification(report));
}

// PRD38 -- docs/roadmap/PRODUIT-2026-10b.md. "exact" is true only when every
// expected frame was compared and none diverges. The report of the night it
// was found -- 150 frames, 111 under a motion clip over a picture, 39
// compared, none diverging -- used to read "exact": true.
void CheckExactVerdict() {
    ExportVerificationReport report;
    report.expected_frames = 150;
    report.exported_frames = 150;
    report.checked_frames = 39;
    report.unverifiable_frames = 111;
    std::string json = SerializeExportVerification(report);
    Check(
        !ExportVerificationIsExact(report) &&
            json.find("\"exact\":false,\"verified_fraction\":0.26,") !=
                std::string::npos &&
            json.find("\"not_exact_because\":\"111 of the 150 expected "
                      "frames were not compared") != std::string::npos &&
            json.find("none of the 39 compared diverges") != std::string::npos,
        "39 frames compared of 150 is not exact, and says why: " + json);

    // Every frame compared, still shots among them: exact, nothing beside.
    report.checked_frames = 150;
    report.unverifiable_frames = 0;
    report.ambiguous_frames = 20;
    report.unconfirmed_offset_frames = 1;
    json = SerializeExportVerification(report);
    Check(ExportVerificationIsExact(report) &&
              json.find("\"exact\":true,\"verified_fraction\":1,") !=
                  std::string::npos &&
              json.find("not_exact_because") == std::string::npos,
          "every frame compared and none diverging is exact: " + json);

    // One frame short of the whole is not rounded up to it.
    report.checked_frames = 149;
    report.unverifiable_frames = 1;
    json = SerializeExportVerification(report);
    Check(!ExportVerificationIsExact(report) &&
              json.find("\"verified_fraction\":0.993,") != std::string::npos,
          "149 of 150 is 0.993, not 1: " + json);

    // Every frame compared and some diverging: verified, and wrong.
    report.checked_frames = 150;
    report.unverifiable_frames = 0;
    report.divergent_frames = 3;
    json = SerializeExportVerification(report);
    Check(!ExportVerificationIsExact(report) &&
              json.find("\"verified_fraction\":1,") != std::string::npos &&
              json.find("\"not_exact_because\":\"3 frames diverge") !=
                  std::string::npos,
          "divergent frames are named as the reason: " + json);

    // A file of the wrong length, and a timeline with nothing to compare.
    report.divergent_frames = 0;
    report.exported_frames = 149;
    report.checked_frames = 149;
    json = SerializeExportVerification(report);
    Check(!ExportVerificationIsExact(report) &&
              json.find("the file has 149 frames where the timeline has 150") !=
                  std::string::npos,
          "a frame missing from the file is the reason: " + json);
    ExportVerificationReport empty;
    Check(!ExportVerificationIsExact(empty) &&
              SerializeExportVerification(empty).find(
                  "\"verified_fraction\":0,") != std::string::npos,
          "nothing compared is never exact: " +
              SerializeExportVerification(empty));
}

// OR4 -- docs/roadmap/DONNEES-2026-10.md. check_timeline on a delivered
// file: the same film exported with and without a closing fade to black,
// over a pale grey on which a plain white carton cannot be read. The file's
// last frames are measured by FFmpeg as the bench measured them, and the
// carton by check_caption_legibility's own measure.
// PA2 -- docs/roadmap/PARITE-2026-11.md. A delivery brought to a loudness:
// the file comes out at the target as a meter reads it, its sound encoded
// once, whether the film is rendered at once or by segments; without a
// target it comes out as the mix stood, as it always did.
void CheckLoudnessTarget(const std::filesystem::path& directory) {
    const auto rush = directory / "loudness-rush.mp4";
    // A sine at an eighth of full scale: about -21 LUFS, well under any
    // delivery target, with seven decibels of gain to find.
    Check(std::system((Quote(FFMPEG_EXECUTABLE) +
                       " -hide_banner -loglevel error -f lavfi -i "
                       "'testsrc2=size=320x180:rate=25:duration=6' -f lavfi "
                       "-i 'sine=frequency=440:sample_rate=48000:duration=6' "
                       "-c:v libx264 -pix_fmt yuv420p -c:a aac -ac 2 "
                       "-shortest -y " +
                       Quote(rush.string()))
                          .c_str()) == 0,
          "loudness: the rush renders");
    Document document;
    document.sequence.width = 320;
    document.sequence.height = 180;
    document.sequence.frame_rate = {25, 1};
    const Ulid sourceId = "01KPA200000000000000000001";
    document.sources = {{sourceId, rush.string(), {25, 1}, {150, 25}}};
    const auto clip = [&](const std::string& id, int64_t in, int64_t source,
                          int64_t frames, bool audio) {
        DocumentClip made;
        made.id = id;
        made.source_id = sourceId;
        made.timeline_in = {in, 25};
        made.source_in = {source, 25};
        made.duration = {frames, 25};
        made.include_audio = audio;
        return made;
    };
    document.sequence.tracks = {
        {"01KPA2000000000000000000T0",
         "video",
         0,
         {clip("01KPA20000000000000000000A", 0, 0, 50, false),
          clip("01KPA20000000000000000000B", 50, 60, 50, false)}},
        {"01KPA2000000000000000000T1",
         "audio",
         1,
         {clip("01KPA20000000000000000000C", 0, 0, 50, true),
          clip("01KPA20000000000000000000D", 50, 60, 50, true)}}};

    const auto deliver = [&](const std::string& name, bool target,
                             int32_t inputsPerPass, ExportPlan& plan) {
        ExportSettings settings =
            Exporter::H264Preset((directory / (name + ".mp4")).string());
        settings.encoder = ExportEncoder::H264Software;
        settings.width = 320;
        settings.height = 180;
        settings.frame_rate = {25, 1};
        settings.ffmpeg_path = FFMPEG_EXECUTABLE;
        settings.max_inputs_per_pass = inputsPerPass;
        if (target) {
            settings.loudness.requested = true;
            settings.loudness.integrated_millilufs = -14000;
        }
        std::string error;
        const bool done =
            Exporter::BuildPlan(document, (directory / "p").string(), settings,
                                plan, error) &&
            Exporter::Run(plan, {}, nullptr, error);
        Check(done, "loudness: " + name + " exports: " + error);
        return done;
    };
    const auto measure = [&](const ExportPlan& plan) {
        FileLoudness loudness;
        std::string error;
        Check(MeasureFileLoudness(plan.settings.output_path, FFMPEG_EXECUTABLE,
                                  loudness, error) &&
                  loudness.integrated_lufs.has_value(),
              "loudness: the delivered file is measured: " + error);
        return loudness.integrated_lufs.value_or(0.0);
    };
    const auto audioCodec = [&](const ExportPlan& plan) -> std::string {
        AVFormatContext* format = nullptr;
        if (avformat_open_input(&format, plan.settings.output_path.c_str(),
                                nullptr, nullptr) < 0)
            return "unreadable";
        std::string name = "none";
        if (avformat_find_stream_info(format, nullptr) >= 0) {
            for (unsigned index = 0; index < format->nb_streams; ++index) {
                const AVCodecParameters* parameters =
                    format->streams[index]->codecpar;
                if (parameters->codec_type == AVMEDIA_TYPE_AUDIO)
                    name = avcodec_get_name(parameters->codec_id);
            }
        }
        avformat_close_input(&format);
        return name;
    };

    ExportPlan plain;
    if (!deliver("loudness-plain", false, 0, plain)) return;
    const double asMixed = measure(plain);
    Check(!plain.loudness_outcome && Exporter::ResultJson(plain).find(
                                         "\"loudness\":") == std::string::npos,
          "loudness: an export with no target reports none");
    Check(asMixed < -19.0 && asMixed > -24.0,
          "loudness: the mix as it stands is the sine's own level: " +
              std::to_string(asMixed));

    ExportPlan levelled;
    if (!deliver("loudness-target", true, 0, levelled)) return;
    const double delivered = measure(levelled);
    Check(std::fabs(delivered + 14.0) <= 0.5,
          "loudness: the delivery is at its target: " +
              std::to_string(delivered));
    Check(audioCodec(levelled) == "aac" && audioCodec(plain) == "aac",
          "loudness: the delivered sound is AAC, as without a target: " +
              audioCodec(levelled));
    Check(
        levelled.loudness_outcome && levelled.loudness_outcome->applied &&
            !levelled.loudness_outcome->silent &&
            levelled.loudness_outcome->mix_lufs &&
            std::fabs(*levelled.loudness_outcome->mix_lufs - asMixed) <= 0.3 &&
            std::fabs(levelled.loudness_outcome->gain_db -
                      (-14.0 - *levelled.loudness_outcome->mix_lufs)) < 1e-9 &&
            levelled.loudness_outcome->delivered_lufs &&
            std::fabs(*levelled.loudness_outcome->delivered_lufs - delivered) <=
                0.11,
        "loudness: the outcome holds the mix, the gain and the result");
    const std::string result = Exporter::ResultJson(levelled);
    Check(result.find("\"loudness\":{\"target_lufs\":-14,\"silent\":false,"
                      "\"mix_lufs\":") != std::string::npos &&
              result.find("\"delivered_lufs\":") != std::string::npos,
          "loudness: the answer says what was measured: " + result);
    Check(!std::filesystem::exists(directory / ".loudness-target-level.mp4"),
          "loudness: nothing is left beside the delivery");
    {
        // The picture is copied, not encoded again: frame for frame what
        // the export without a target wrote.
        ExportVerificationSettings verification;
        verification.ffmpeg_path = FFMPEG_EXECUTABLE;
        ExportVerificationReport report;
        std::string error;
        Check(VerifyExport(document, directory, levelled.settings.output_path,
                           verification, report, error) &&
                  report.exported_frames == 100 && report.divergent_frames == 0,
              "loudness: the levelled file holds the film's frames: " + error +
                  ' ' + SerializeExportVerification(report));
    }

    // One input per pass: the film is rendered by segments and assembled,
    // and the assembly is what is brought to its loudness.
    ExportPlan segmented;
    if (!deliver("loudness-segments", true, 1, segmented)) return;
    Check(!segmented.segments.empty(),
          "loudness: the film was rendered by segments");
    const double assembled = measure(segmented);
    Check(std::fabs(assembled + 14.0) <= 0.5 && audioCodec(segmented) == "aac",
          "loudness: a film rendered by segments is at its target too: " +
              std::to_string(assembled));

    // A silent film has no loudness to bring anywhere: delivered, and said.
    Document silent = document;
    silent.sequence.tracks.pop_back();
    document = silent;
    ExportPlan quiet;
    if (!deliver("loudness-silent", true, 0, quiet)) return;
    Check(quiet.loudness_outcome && quiet.loudness_outcome->applied &&
              quiet.loudness_outcome->silent &&
              quiet.loudness_outcome->gain_db == 0.0 &&
              audioCodec(quiet) == "aac",
          "loudness: a silent film is delivered as it is, and says so");

    ExportSettings refused =
        Exporter::H264Preset((directory / "loudness-refused.mp4").string());
    refused.width = 320;
    refused.height = 180;
    refused.frame_rate = {25, 1};
    refused.loudness.requested = true;
    refused.loudness.integrated_millilufs = -4000;
    ExportPlan plan;
    std::string error;
    Check(!Exporter::BuildPlan(document, (directory / "p").string(), refused,
                               plan, error) &&
              error == "the loudness target must lie between -40 and -5 LUFS",
          "loudness: a target out of range is refused: " + error);
}

void CheckTimelineOnDeliveredFile(const std::filesystem::path& directory) {
    const auto rush = directory / "check-rush.mp4";
    Check(std::system((Quote(FFMPEG_EXECUTABLE) +
                       " -hide_banner -loglevel error -f lavfi -i "
                       "'color=c=0xC0C0C0:s=320x180:rate=25:duration=2' "
                       "-pix_fmt yuv420p -c:v libx264 -crf 0 -y " +
                       Quote(rush.string()))
                          .c_str()) == 0,
          "check: the pale rush renders");
    Document document;
    document.sequence.width = 320;
    document.sequence.height = 180;
    document.sequence.frame_rate = {25, 1};
    document.sources = {
        {"01K9D000000000000000000001", rush.string(), {25, 1}, {50, 25}}};
    DocumentClip picture;
    picture.id = "01K9D00000000000000000000A";
    picture.source_id = document.sources[0].id;
    picture.source_in = {0, 25};
    picture.duration = {40, 25};
    picture.timeline_in = {0, 25};
    picture.include_audio = false;
    CaptionStyle carton;
    carton.font_size = 36;
    DocumentClip text;
    text.id = "01K9D00000000000000000000B";
    text.timeline_in = {5, 25};
    text.duration = {10, 25};
    text.include_audio = false;
    text.caption_group_id = carton.id;
    text.caption_text = "PALE";
    document.sequence.caption_styles = {carton};
    document.sequence.tracks = {
        {"01K9D0000000000000000000T0", "video", 0, {picture}},
        {"01K9D0000000000000000000T1", "caption", 1, {text}}};
    DocumentTransition closing;
    closing.id = "01K9D0000000000000000000X1";
    closing.track_id = document.sequence.tracks[0].id;
    closing.left_clip_id = picture.id;
    closing.type = "dip_to_color";
    closing.duration = {10, 25};
    closing.alignment = TransitionAlignment::EndAtCut;

    const auto deliver = [&](const Document& film, const std::string& name,
                             TimelineCheckReport& report) {
        ExportSettings settings =
            Exporter::H264Preset((directory / (name + ".mp4")).string());
        settings.encoder = ExportEncoder::H264Software;
        settings.width = 320;
        settings.height = 180;
        settings.frame_rate = {25, 1};
        settings.ffmpeg_path = FFMPEG_EXECUTABLE;
        ExportPlan plan;
        std::string failure;
        if (!Exporter::BuildPlan(film, (directory / "check.json").string(),
                                 settings, plan, failure) ||
            !Exporter::Run(plan, {}, nullptr, failure)) {
            Check(false, name + " export: " + failure);
            return false;
        }
        TimelineFileMeasures measures;
        if (!MeasureDeliveredFile(film, TimelinePolicy{}, settings.output_path,
                                  FFMPEG_EXECUTABLE, measures, failure) ||
            !CheckTimeline(film, TimelinePolicy{}, &measures, report,
                           failure)) {
            Check(false, name + " check: " + failure);
            return false;
        }
        return true;
    };
    const auto status = [](const TimelineCheckReport& report,
                           const std::string& name) {
        for (const TimelineCheckResult& check : report.checks)
            if (check.name == name) return check.status;
        return TimelineCheckStatus::NotRun;
    };

    TimelineCheckReport plain;
    if (deliver(document, "check-plain", plain)) {
        const std::string json = SerializeTimelineCheck(plain);
        Check(status(plain, "final_black") == TimelineCheckStatus::Passed &&
                  json.find("\"document\":[],\"file_frames\":25,\"file\":[]") !=
                      std::string::npos,
              "check: a film that ends on its picture has no black, in the "
              "document or in its last 25 frames: " +
                  json);
        Check(
            status(plain, "caption_contrast") == TimelineCheckStatus::Failed &&
                json.find("\"cartons\":1,\"below_minimum\":[{\"clip_id\":"
                          "\"01K9D00000000000000000000B\",\"text\":"
                          "\"PALE\",\"frame\":10,") != std::string::npos,
            "check: white on pale grey is under 3:1: " + json);
    }

    Document faded = document;
    faded.sequence.transitions = {closing};
    std::string validation;
    Check(faded.Validate(validation),
          "check: the closing fade validates: " + validation);
    TimelineCheckReport fade;
    if (deliver(faded, "check-fade", fade)) {
        const std::string json = SerializeTimelineCheck(fade);
        Check(status(fade, "final_black") == TimelineCheckStatus::Failed &&
                  json.find("\"document\":[{\"from_frame\":31,\"until_frame\":"
                            "40,\"kind\":\"fading\",\"cause\":"
                            "\"dip_to_color\"") != std::string::npos,
              "check: the fade is read on the document, from its first "
              "visible frame: " +
                  json);
        // The veil reaches 9/10 on the last frame: the pale grey at a
        // tenth is under blackdetect's tenth of full scale, so the file
        // ends on frames FFmpeg calls black.
        const size_t file = json.find("\"file\":[{\"from_frame\":");
        Check(file != std::string::npos &&
                  json.find("\"until_frame\":40}]", file) != std::string::npos,
              "check: and in the file, up to its last frame: " + json);
    }
}

// Decided on 8 October 2026 (ROADMAP.md): a caption family the caption
// renderer lacks is named in the export's answer, with what replaces it;
// an answer with nothing to say keeps its fields.
void CheckCaptionFontAnswer() {
    ExportPlan plan;
    plan.settings = Exporter::H264Preset("/tmp/film.mp4");
    const std::string plain = Exporter::ResultJson(plan);
    Check(plain.find("caption_fonts_substituted") == std::string::npos,
          "no substitution: the answer has no caption_fonts_substituted");
    plan.caption_font_substitutions = {{"Poppins", "Helvetica Neue"}};
    const std::string said = Exporter::ResultJson(plan);
    Check(said.find(R"("caption_fonts_substituted":[{"family":"Poppins",)"
                    R"("exported_as":"Helvetica Neue"}])") != std::string::npos,
          "a substituted family is in the answer: " + said);
}

// DEC31, BH2 -- docs/roadmap/BLANC-HLG-2026-10.md, and R4, criteria 1 and 2
// -- docs/roadmap/REEL-2026-09.md. A PNG logo with alpha -- a red box and a
// white one on transparency -- ingested as a still, held 50 frames at speed
// 0/1 on V2 over a grey rush. Declared a graphic, its code values are what
// the file shows, as a motion title's are: exactly without colour
// management, through the inverse of the SDR view under it (cube sampling
// apart), at the peak of an HLG delivery. Read as a camera's Rec.709 it is
// not: the measure that BH2 asked for, on a synthetic logo.
void CheckGraphicsExport(const std::filesystem::path& directory) {
    constexpr int kWidth = 320;
    constexpr int kHeight = 180;
    const auto logo = directory / "graphics-logo.png";
    const std::string drawLogo =
        Quote(FFMPEG_EXECUTABLE) +
        " -hide_banner -loglevel error -f lavfi -i "
        "'color=c=black@0:s=320x180,format=rgba,"
        "drawbox=x=40:y=46:w=100:h=88:color=0xE07060@1:t=fill,"
        "drawbox=x=180:y=46:w=100:h=88:color=0xFFFFFF@1:t=fill,"
        // drawbox leaves the alpha it drew over: the boxes are made opaque.
        "geq=r=r(X\\,Y):g=g(X\\,Y):b=b(X\\,Y):"
        "a=if(gt(r(X\\,Y)+g(X\\,Y)+b(X\\,Y)\\,0)\\,255\\,0)' "
        "-frames:v 1 -y " +
        Quote(logo.string());
    const auto grey = directory / "graphics-grey.mp4";
    Check(std::system(drawLogo.c_str()) == 0 &&
              WriteFlatSource(grey, kWidth, kHeight, "0x808080"),
          "graphics: the logo and the rush are generated");
    // R4, criterion 1 -- a still is one frame, which a clip holds at 0/1.
    LibraryMedia still;
    std::string error;
    Check(ProbeMediaMetadata(logo.string(), still, error) && still.has_video &&
              !still.has_audio && still.width == kWidth &&
              still.height == kHeight &&
              still.duration.value == still.rate.den &&
              still.duration.rate == still.rate.num &&
              still.pixel_format == "rgba",
          "graphics: a PNG probes as a one-frame picture with alpha (" +
              still.pixel_format + ", " + std::to_string(still.duration.value) +
              '/' + std::to_string(still.duration.rate) + "): " + error);
    still.id = "01K9G0000000000000000000A1";
    still.path = logo.string();
    still.filename = logo.filename().string();
    LibraryMedia rush;
    Check(ProbeMediaMetadata(grey.string(), rush, error),
          "graphics: the rush probes: " + error);
    rush.id = "01K9G0000000000000000000R1";
    rush.path = grey.string();
    rush.filename = grey.filename().string();

    const auto documentFor = [&](const ColorManagementSettings& color,
                                 bool graphic) {
        Document document;
        document.sequence.width = kWidth;
        document.sequence.height = kHeight;
        document.sequence.frame_rate = {25, 1};
        document.color_management = color;
        LibraryMedia logoMedia = still;
        if (graphic) logoMedia.color_input.graphics = true;
        document.library = {rush, logoMedia};
        document.sources = {
            {rush.id, rush.path, rush.rate, rush.duration},
            {logoMedia.id, logoMedia.path, logoMedia.rate, logoMedia.duration}};
        DocumentClip shot;
        shot.id = "01K9G0000000000000000000C1";
        shot.source_id = rush.id;
        shot.source_in = {0, 25};
        shot.duration = {50, 25};
        shot.timeline_in = {0, 25};
        shot.include_audio = false;
        DocumentClip held;
        held.id = "01K9G0000000000000000000C2";
        held.source_id = logoMedia.id;
        held.source_in = {0, still.rate.num};
        held.duration = {50, 25};
        held.timeline_in = {0, 25};
        held.include_audio = false;
        held.speed = {0, 1};
        document.sequence.tracks = {
            {"01K9G0000000000000000000T1", "video", 0, {shot}},
            {"01K9G0000000000000000000T2", "video", 1, {held}}};
        return document;
    };
    struct Read {
        bool ok = false;
        FloatFrames frames;
    };
    const auto exportOf = [&](const Document& document, const std::string& name,
                              bool hlg) {
        Read read;
        std::string validation;
        Check(document.Validate(validation),
              "graphics " + name + ": the timeline validates: " + validation);
        const auto output = directory / ("graphics-" + name + ".mp4");
        ExportSettings settings =
            hlg ? Exporter::HevcMain10SoftwarePreset(output.string())
                : Exporter::H264Preset(output.string());
        if (!hlg) settings.encoder = ExportEncoder::H264Software;
        settings.width = kWidth;
        settings.height = kHeight;
        settings.frame_rate = {25, 1};
        settings.ffmpeg_path = FFMPEG_EXECUTABLE;
        ExportPlan plan;
        std::string exportError;
        const bool exported =
            Exporter::BuildPlan(
                document, (directory / ("graphics-" + name + ".json")).string(),
                settings, plan, exportError) &&
            Exporter::Run(plan, {}, nullptr, exportError);
        Check(exported, "graphics " + name + ": it exports: " + exportError);
        read.ok =
            exported &&
            ReadFloatFrames(output.string(), kWidth, kHeight, read.frames) &&
            read.frames.Count() == 50;
        Check(!exported || read.ok,
              "graphics " + name + ": the export decodes to 50 frames");
        return read;
    };
    const RgbColor greyCode{128.0 / 255.0, 128.0 / 255.0, 128.0 / 255.0};
    const RgbColor redCode{224.0 / 255.0, 112.0 / 255.0, 96.0 / 255.0};
    const RgbColor white{1.0, 1.0, 1.0};

    // Without colour management: the held logo on every frame, its alpha
    // kept, the red written as the file has it, and both sides of each
    // edge read back -- the box spans columns 40 to 139, rows 46 to 133.
    const Read unmanaged = exportOf(
        documentFor(ColorManagementSettings{}, true), "unmanaged", false);
    if (unmanaged.ok) {
        for (size_t frame : {size_t{0}, size_t{49}}) {
            const std::string at = " (frame " + std::to_string(frame) + ")";
            Check(MaxChannelDistance(unmanaged.frames.At(frame, 90, 90),
                                     redCode) <= 0.02,
                  "graphics unmanaged: the red box as written" + at + ", " +
                      Describe(unmanaged.frames.At(frame, 90, 90)));
            Check(MaxChannelDistance(unmanaged.frames.At(frame, 230, 90),
                                     white) <= 0.02,
                  "graphics unmanaged: the white box at white" + at + ", " +
                      Describe(unmanaged.frames.At(frame, 230, 90)));
            Check(MaxChannelDistance(unmanaged.frames.At(frame, 10, 10),
                                     greyCode) <= 0.02,
                  "graphics unmanaged: the rush through the transparency" + at +
                      ", " + Describe(unmanaged.frames.At(frame, 10, 10)));
        }
        // R4, criterion 2 -- the edge of the logo, both sides, in luma:
        // the column and the row before the box show the rush, the first
        // ones inside it the box.
        const auto luma = [](const RgbColor& color) {
            return 0.2126 * color.red + 0.7152 * color.green +
                   0.0722 * color.blue;
        };
        const double greyLuma = luma(greyCode);
        const double whiteLuma = 1.0;
        Check(std::abs(luma(unmanaged.frames.At(0, 179, 90)) - greyLuma) <=
                      0.02 &&
                  std::abs(luma(unmanaged.frames.At(0, 180, 90)) - whiteLuma) <=
                      0.03 &&
                  std::abs(luma(unmanaged.frames.At(0, 230, 45)) - greyLuma) <=
                      0.02 &&
                  std::abs(luma(unmanaged.frames.At(0, 230, 46)) - whiteLuma) <=
                      0.03,
              "graphics unmanaged: the white box's edge falls on its pixel, "
              "left " +
                  Describe(unmanaged.frames.At(0, 179, 90)) + " | " +
                  Describe(unmanaged.frames.At(0, 180, 90)) + ", top " +
                  Describe(unmanaged.frames.At(0, 230, 45)) + " | " +
                  Describe(unmanaged.frames.At(0, 230, 46)));
    }
    // The same PNG read as a camera's Rec.709 picture, unmanaged: printed,
    // not judged -- it is what an RGB rush gets.
    const Read camera = exportOf(documentFor(ColorManagementSettings{}, false),
                                 "camera", false);
    if (camera.ok)
        std::cout << "graphics: the PNG as a camera picture, unmanaged: red "
                  << Describe(camera.frames.At(0, 90, 90)) << ", white "
                  << Describe(camera.frames.At(0, 230, 90)) << '\n';

    // Under colour management, a Rec.709 project to an SDR delivery.
    ColorManagementSettings managed;
    managed.enabled = true;
    const Read graphicSdr = exportOf(documentFor(managed, true), "sdr", false);
    const Read cameraSdr =
        exportOf(documentFor(managed, false), "sdr-camera", false);
    if (graphicSdr.ok && cameraSdr.ok) {
        const RgbColor red = graphicSdr.frames.At(0, 90, 90);
        const RgbColor whiteShown = graphicSdr.frames.At(0, 230, 90);
        const RgbColor cameraWhite = cameraSdr.frames.At(0, 230, 90);
        std::cout << "graphics SDR: red " << Describe(red) << ", white "
                  << Describe(whiteShown) << "; as a camera, red "
                  << Describe(cameraSdr.frames.At(0, 90, 90)) << ", white "
                  << Describe(cameraWhite) << '\n';
        Check(MaxChannelDistance(red, redCode) <= 0.03 &&
                  MaxChannelDistance(whiteShown, white) <= 0.03,
              "graphics SDR: the graphic shows its code values, red " +
                  Describe(red) + ", white " + Describe(whiteShown));
        Check(cameraWhite.green < 0.95,
              "graphics SDR: read as a camera, the white falls under white "
              "(" +
                  Describe(cameraWhite) + "): the declaration is what moved");
    }
    // An HLG delivery: the graphic's white at the peak (BH1's 0.994), the
    // camera's well under it.
    ColorManagementSettings hlg = managed;
    hlg.output_gamut = "rec2020";
    hlg.output_transfer = "hlg";
    const Read graphicHlg = exportOf(documentFor(hlg, true), "hlg", true);
    const Read cameraHlg =
        exportOf(documentFor(hlg, false), "hlg-camera", true);
    if (graphicHlg.ok && cameraHlg.ok) {
        const RgbColor whiteShown = graphicHlg.frames.At(0, 230, 90);
        const RgbColor cameraWhite = cameraHlg.frames.At(0, 230, 90);
        std::cout << "graphics HLG: white " << Describe(whiteShown)
                  << "; as a camera, white " << Describe(cameraWhite) << '\n';
        Check(whiteShown.green >= 0.98,
              "graphics HLG: the graphic's white reaches the peak, " +
                  Describe(whiteShown));
        Check(cameraWhite.green < whiteShown.green - 0.1,
              "graphics HLG: read as a camera, its white stays under the "
              "peak, " +
                  Describe(cameraWhite));
    }
    // A grade on a graphic under colour management is refused by name, not
    // dropped.
    Document graded = documentFor(managed, true);
    ClipEffect exposure;
    exposure.type = "color.exposure";
    exposure.params["amount"] = {1, 2};
    graded.sequence.tracks[1].clips[0].effects = {exposure};
    ExportSettings refusedSettings =
        Exporter::H264Preset((directory / "graphics-graded.mp4").string());
    refusedSettings.encoder = ExportEncoder::H264Software;
    refusedSettings.width = kWidth;
    refusedSettings.height = kHeight;
    refusedSettings.frame_rate = {25, 1};
    refusedSettings.ffmpeg_path = FFMPEG_EXECUTABLE;
    ExportPlan refusedPlan;
    std::string refusal;
    Check(!Exporter::BuildPlan(graded,
                               (directory / "graphics-graded.json").string(),
                               refusedSettings, refusedPlan, refusal) &&
              refusal.find("is a graphic") != std::string::npos,
          "graphics: a graded graphic under colour management is refused "
          "by name: " +
              refusal);
}

int main() {
    // LG2, LG3 -- the cartons are measured and drawn by Core Text.
    InstallCaptionRenderer();
    CheckExactVerdict();
    CheckCaptionFontAnswer();
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() /
        (GenerateUlid() + "-export-tests");
    std::filesystem::create_directories(directory);
    const std::filesystem::path media = directory / "source.mp4";
    const std::filesystem::path project = directory / "project.json";
    const std::filesystem::path destination = directory / "result.mp4";

    const std::string generate =
        Quote(FFMPEG_EXECUTABLE) +
        " -hide_banner -loglevel error -f lavfi -i "
        "'testsrc2=size=320x180:rate=25:duration=1' -f lavfi -i "
        "'sine=frequency=440:sample_rate=48000:duration=1' -c:v libx264 "
        "-pix_fmt yuv420p -c:a aac -ac 2 -shortest -y " +
        Quote(media.string());
    Check(std::system(generate.c_str()) == 0, "source fixture is generated");
    CheckOfflineExportInputs(directory, media.string());

    // ITW1 -- the simulation must keep describing the defect measured on the
    // interview edit, or the positions chosen below would prove nothing.
    Check(OldPlacementTruncates(232, {25, 1}) &&
              OldPlacementTruncates(426, {25, 1}) &&
              OldPlacementTruncates(929, {25, 1}) &&
              !OldPlacementTruncates(231, {25, 1}) &&
              OldWindowOverflows(930, 122, {25, 1}),
          "float placement simulation reproduces the measured truncations");
    CheckFrameExactExport(directory, {25, 1}, "25");
    CheckFrameExactExport(directory, {24000, 1001}, "23.976");
    CheckFrameExactExport(directory, {50, 1}, "50");
    CheckMulticamExport(directory);
    CheckFrameExactExport(directory, {24000, 1001}, "23.976-managed", true);
    CheckLinearManagedComposite(directory);
    CheckWheelsAndCurvesExport(directory);
    CheckUnmanagedArgumentsUnchanged(directory, media);
    CheckSpeedExport(directory);
    CheckFreezeExport(directory);
    CheckGraphicsExport(directory);
    CheckRampExport(directory);
    CheckSlowedAudioExport(directory);
    CheckFramingExport(directory);
    CheckAnimatedFramingExport(directory);
    CheckAnimatedClipAfterDissolve(directory);
    CheckLongAnimationExport(directory);
    CheckWindowRender(directory);
    CheckAnimatedOpacityExport(directory);
    CheckColorTransitionExport(directory);
    CheckFramingVerification(directory);
    CheckTimelineOnDeliveredFile(directory);
    CheckLoudnessTarget(directory);

    {
        // A locked-off shot gives no picture evidence of an offset: the
        // verification must count it ambiguous and report nothing.
        const auto still = directory / "still.mp4";
        Check(WriteNumberedSource(still, {25, 1}, 60, true),
              "still source is generated");
        Document stillDocument;
        stillDocument.sequence.width = kCodeWidth;
        stillDocument.sequence.height = kCodeHeight;
        stillDocument.sources = {
            {"01K4F7W1000000000000000009", still.string(), {25, 1}, {60, 25}}};
        DocumentClip stillClip;
        stillClip.source_id = stillDocument.sources[0].id;
        stillClip.source_in = {5, 25};
        stillClip.duration = {40, 25};
        stillClip.timeline_in = {0, 25};
        stillDocument.sequence.tracks = {
            {"01K4F7W100000000000000000A", "video", 0, {stillClip}}};
        ExportSettings stillSettings =
            Exporter::H264Preset((directory / "still-export.mp4").string());
        stillSettings.encoder = ExportEncoder::H264Software;
        stillSettings.width = kCodeWidth;
        stillSettings.height = kCodeHeight;
        stillSettings.frame_rate = {25, 1};
        stillSettings.ffmpeg_path = FFMPEG_EXECUTABLE;
        ExportPlan stillPlan;
        std::string stillError;
        Check(Exporter::BuildPlan(stillDocument, (directory / "p").string(),
                                  stillSettings, stillPlan, stillError) &&
                  Exporter::Run(stillPlan, {}, nullptr, stillError),
              "still timeline exports: " + stillError);
        stillDocument.sequence.tracks[0].clips[0].source_in = {6, 25};
        ExportVerificationSettings verification;
        verification.ffmpeg_path = FFMPEG_EXECUTABLE;
        ExportVerificationReport stillReport;
        Check(VerifyExport(stillDocument, directory, stillSettings.output_path,
                           verification, stillReport, stillError) &&
                  stillReport.divergent_frames == 0 &&
                  stillReport.ambiguous_frames == 40,
              "a still shot is ambiguous, never reported as shifted: " +
                  stillError + ' ' + SerializeExportVerification(stillReport));
    }

    Document document = Fixture(media.string());
    document.sequence.tracks[0].clips[0].opacity = {1, 2};
    document.sequence.tracks[2].clips[0].audio_gain_db = {10, 1};
    document.sequence.tracks[2].clips[0].audio_fade_in = {2, 25};
    document.sequence.tracks[2].clips[0].audio_fade_out = {3, 25};
    std::string error;
    Check(document.Save(project.string(), error), "project saves: " + error);

    ExportSettings settings =
        Exporter::HevcMain10SoftwarePreset(destination.string());
    settings.width = 320;
    settings.height = 180;
    settings.frame_rate = {25, 1};
    settings.ffmpeg_path = FFMPEG_EXECUTABLE;
    ExportPlan plan;
    Check(
        Exporter::BuildPlan(document, project.string(), settings, plan, error),
        "export plan builds: " + error);
    Check(plan.total_frames == 15,
          "timeline duration is converted to an exact frame count");
    // Agentic QA of 23 September 2026 -- a rush the timeline does not use
    // may be offline; one it uses may not.
    {
        Document offline = document;
        offline.sources.push_back({"01K4000000000000000000000Z",
                                   (directory / "offline.mp4").string(),
                                   {25, 1},
                                   {25, 25}});
        ExportPlan offlinePlan;
        std::string offlineError;
        Check(Exporter::BuildPlan(offline, project.string(), settings,
                                  offlinePlan, offlineError),
              "an offline rush no clip reads does not stop the export: " +
                  offlineError);
        offline.sequence.tracks[0].clips[0].source_id =
            "01K4000000000000000000000Z";
        Check(
            !Exporter::BuildPlan(offline, project.string(), settings,
                                 offlinePlan, offlineError) &&
                offlineError.find("offline.mp4") != std::string::npos,
            "an offline rush a clip reads is refused by name: " + offlineError);
    }
    Check(plan.filter_graph.find("overlay=") != std::string::npos &&
              plan.filter_graph.find("amix=inputs=1") != std::string::npos,
          "plan contains video composition and audio mixing");
    Check(plan.filter_graph.find("alimiter=limit=0.668344:level=0:latency=1") !=
              std::string::npos,
          "AAC export keeps codec headroom without automatic makeup gain");
    Check(plan.filter_graph.find("eof_action=repeat:repeatlast=1") !=
                  std::string::npos &&
              plan.filter_graph.find("enable=gte(t\\,") != std::string::npos &&
              plan.filter_graph.find(")*lt(t\\,") != std::string::npos,
          "video clips hold their final decoded frame only inside their "
          "half-open timeline range");
    Check(
        plan.filter_graph.find("setpts=PTS-STARTPTS+5[") != std::string::npos &&
            plan.filter_graph.find("enable=gte(t\\,((9*1)/50))*"
                                   "lt(t\\,((29*1)/50))") !=
                std::string::npos &&
            plan.filter_graph.find("/TB") == std::string::npos,
        "clip placement is an integer frame count with half-frame enable "
        "bounds, never a float quotient");
    Check(plan.filter_graph.find("colorchannelmixer=aa=(1/2)") !=
              std::string::npos,
          "opaque export composites the clip's exact opacity");
    Check(plan.filter_graph.find("volume=pow(10\\,(10/1)/20)") !=
                  std::string::npos &&
              plan.filter_graph.find("afade=t=in:st=0:d=0.08") !=
                  std::string::npos &&
              plan.filter_graph.find("afade=t=out:st=0.28:d=0.12") !=
                  std::string::npos,
          "audio export applies exact clip gain and fade envelope");

    // EF2 -- docs/roadmap/EFFETS-2026-10.md. A cross fade reads the sound
    // past its cut: the incoming clip starts two frames early, the outgoing
    // one ends two frames late, and both ramp over the four on a quarter
    // sine.
    {
        Document faded = Fixture(media.string());
        DocumentTrack& sound = faded.sequence.tracks[2];
        DocumentClip next = sound.clips.front();
        next.id = "01K40000000000000000000009";
        next.source_in = {12, 25};
        next.duration = {8, 25};
        next.timeline_in = {15, 25};
        sound.clips.push_back(next);
        faded.sequence.transitions.push_back({"01K4000000000000000000000A",
                                              sound.id,
                                              sound.clips[0].id,
                                              next.id,
                                              "cross_fade",
                                              {4, 25},
                                              TransitionAlignment::Center});
        ExportPlan fadedPlan;
        std::string fadedError;
        Check(
            Exporter::BuildPlan(faded, project.string(), settings, fadedPlan,
                                fadedError) &&
                fadedPlan.filter_graph.find(
                    "afade=t=out:st=0.32:d=0.16:curve=qsin") !=
                    std::string::npos &&
                fadedPlan.filter_graph.find(
                    "atrim=duration=0.4,asetpts=PTS-STARTPTS,aresample="
                    "48000,aformat=sample_fmts=fltp:channel_layouts=stereo,"
                    "afade=t=in:st=0:d=0.16:curve=qsin") != std::string::npos &&
                fadedPlan.filter_graph.find("adelay=24960S") !=
                    std::string::npos,
            "a cross fade extends both sounds over its handles and ramps "
            "them on a quarter sine: " +
                fadedError);
    }

    // EF3 -- docs/roadmap/EFFETS-2026-10.md. A blurred clip's chain draws
    // `gblur` on its picture, at its size in the frame and before it is
    // laid there; the sigma is carried from sequence pixels into the
    // delivery's exactly, axis by axis, and every other chain keeps its
    // bytes.
    {
        Document sharp = Fixture(media.string());
        Document blurred = sharp;
        blurred.sequence.tracks[1].clips[0].blur = {12, 1};
        ExportSettings blurSettings = settings;
        blurSettings.output_path = (directory / "blurred.mp4").string();
        ExportPlan sharpPlan;
        ExportPlan blurredPlan;
        std::string blurError;
        const bool built =
            Exporter::BuildPlan(sharp, project.string(), blurSettings,
                                sharpPlan, blurError) &&
            Exporter::BuildPlan(blurred, project.string(), blurSettings,
                                blurredPlan, blurError);
        Check(built, "a blurred clip's plan builds: " + blurError);
        // 12 sequence pixels of a 1920 x 1080 sequence, in a 320 x 180
        // delivery: 2 pixels.
        const std::string drawn =
            ",format=yuva444p16le,gblur=sigma=(2/1):steps=6";
        const size_t at = blurredPlan.filter_graph.find(
            "setsar=1,setparams=colorspace=bt709" + drawn + ",setpts=");
        Check(at != std::string::npos &&
                  Count(blurredPlan.filter_graph, "gblur=") == 1,
              "the blurred clip's picture is blurred once, after it is "
              "scaled and before it is laid in the frame: " +
                  blurredPlan.filter_graph);
        std::string unblurred = blurredPlan.filter_graph;
        const size_t drawnAt = unblurred.find(drawn);
        if (drawnAt != std::string::npos)
            unblurred.erase(drawnAt, drawn.size());
        Check(unblurred == sharpPlan.filter_graph,
              "without its blur, the graph is the sharp document's, byte "
              "for byte");

        // Another resolution, another shape: each axis its own sigma.
        ExportSettings square = blurSettings;
        square.width = 320;
        square.height = 320;
        ExportPlan squarePlan;
        Check(Exporter::BuildPlan(blurred, project.string(), square, squarePlan,
                                  blurError) &&
                  squarePlan.filter_graph.find(
                      "gblur=sigma=(2/1):steps=6:sigmaV=(32/9)") !=
                      std::string::npos,
              "a frame of another shape blurs each axis by its own scale: " +
                  blurError);

        // Under colour management the picture is blurred in its own
        // signal, before `pad` lays it on the transparent frame: its edges
        // are extended, never mixed with the border.
        Document managed = blurred;
        managed.color_management.enabled = true;
        managed.color_management.input_gamut = "sony_sgamut3_cine";
        managed.color_management.input_transfer = "sony_slog3";
        ExportPlan managedPlan;
        Check(Exporter::BuildPlan(managed, project.string(), blurSettings,
                                  managedPlan, blurError) &&
                  managedPlan.filter_graph.find(
                      "setsar=1" + drawn +
                      ",format=yuva444p16le,pad=320:180:") != std::string::npos,
              "managed, the blur comes before the layer is padded into the "
              "frame: " +
                  blurError + managedPlan.filter_graph);

        // A sigma FFmpeg cannot take at the delivery's resolution is
        // refused by name, not clamped.
        Document tiny = blurred;
        tiny.sequence.width = 32;
        tiny.sequence.height = 18;
        tiny.sequence.tracks[1].clips[0].blur = {200, 1};
        ExportPlan tinyPlan;
        std::string tinyError;
        Check(!Exporter::BuildPlan(tiny, project.string(), blurSettings,
                                   tinyPlan, tinyError) &&
                  tinyError.find("past the 1024") != std::string::npos,
              "a blur past what gblur takes is refused by name: " + tinyError);
        // Last, since the file it writes is the destination of every plan
        // above: FFmpeg takes the graph and renders it.
        Check(built && Exporter::Run(blurredPlan, {}, nullptr, blurError),
              "FFmpeg renders the blurred clip: " + blurError);

        // A blurred clip whose opacity moves: its picture takes its one size
        // before the blur, and the per-frame scale at the end of its chain
        // changes nothing.
        Document fading = blurred;
        ClipKeyframe from;
        from.id = "01K4000000000000000000K001";
        from.time = {0, 25};
        from.value = {1, 1};
        ClipKeyframe to = from;
        to.id = "01K4000000000000000000K002";
        to.time = {4, 25};
        to.value = {1, 4};
        ClipParameterAnimation fade;
        fade.parameter = ClipAnimatedParameter::Opacity;
        fade.keyframes = {from, to};
        fading.sequence.tracks[1].clips[0].animation = {fade};
        ExportSettings fadingSettings = blurSettings;
        fadingSettings.output_path =
            (directory / "blurred-fading.mp4").string();
        ExportPlan fadingPlan;
        Check(Exporter::BuildPlan(fading, project.string(), fadingSettings,
                                  fadingPlan, blurError) &&
                  fadingPlan.filter_graph.find(
                      "scale=320:180:flags=lanczos,setsar=1,setparams="
                      "colorspace=bt709" +
                      drawn) != std::string::npos &&
                  Exporter::Run(fadingPlan, {}, nullptr, blurError),
              "a blurred clip with a moving opacity is sized, blurred and "
              "rendered: " +
                  blurError + fadingPlan.filter_graph);
    }

    Document outputStateDocument = Fixture(media.string());
    outputStateDocument.sequence.tracks[1].visible = false;
    outputStateDocument.sequence.tracks[2].muted = true;
    ExportPlan outputStatePlan;
    Check(Exporter::BuildPlan(outputStateDocument, project.string(), settings,
                              outputStatePlan, error) &&
              Count(outputStatePlan.filter_graph, "overlay=") == 1 &&
              outputStatePlan.filter_graph.find("amix=") == std::string::npos,
          "export honors video visibility and audio mute state");

    // F1.3 -- decoded delivery pixels must reflect the ordered source grade,
    // while the derived LUT and the canonical document stay disposable.
    Document gradeDocument = Fixture(media.string());
    gradeDocument.sequence.tracks.resize(1);
    auto& gradeClip = gradeDocument.sequence.tracks[0].clips[0];
    gradeClip.timeline_in = {0, 25};
    gradeClip.source_in = {0, 25};
    gradeClip.duration = {25, 25};
    ExportSettings gradeSettings =
        Exporter::H264Preset((directory / "grade-baseline.mp4").string());
    gradeSettings.encoder = ExportEncoder::H264Software;
    gradeSettings.width = 320;
    gradeSettings.height = 180;
    gradeSettings.frame_rate = {25, 1};
    gradeSettings.ffmpeg_path = FFMPEG_EXECUTABLE;
    ExportPlan baselineGradePlan;
    Check(Exporter::BuildPlan(gradeDocument, project.string(), gradeSettings,
                              baselineGradePlan, error) &&
              Exporter::Run(baselineGradePlan, {}, nullptr, error),
          "ungraded baseline renders for pixel comparison: " + error);
    gradeClip.effects = {
        {GenerateUlid(), "color.exposure", {{"amount", {4, 1}}}}};
    const std::string gradeBytes = gradeDocument.SaveToString();
    gradeSettings.output_path = (directory / "grade-exposure.mp4").string();
    ExportPlan gradedPlan;
    Check(Exporter::BuildPlan(gradeDocument, project.string(), gradeSettings,
                              gradedPlan, error) &&
              gradedPlan.clip_grade_luts.size() == 1 &&
              gradedPlan.filter_graph.find("lut3d=file=") <
                  gradedPlan.filter_graph.find("overlay=") &&
              Exporter::Run(gradedPlan, {}, nullptr, error),
          "clip grade renders before composition: " + error);
    std::array<double, 3> baselineMean{};
    std::array<double, 3> gradedMean{};
    Check(MeanFirstRgbFrame(baselineGradePlan.settings.output_path, 320, 180,
                            baselineMean) &&
              MeanFirstRgbFrame(gradedPlan.settings.output_path, 320, 180,
                                gradedMean) &&
              gradedMean[0] > baselineMean[0] + 10.0 &&
              gradedMean[1] > baselineMean[1] + 10.0 &&
              gradedMean[2] > baselineMean[2] + 10.0,
          "decoded exposure pixels brighten in all three channels");
    Check(gradeDocument.SaveToString() == gradeBytes &&
              !gradedPlan.clip_grade_luts.empty() &&
              !std::filesystem::exists(gradedPlan.clip_grade_luts.front().path),
          "graded export preserves the document and removes its LUT");
    // ITW5 -- under colour management the graded clip renders, and its grade
    // shows against the same clip managed without one. F1.3 -- each clip
    // reaches the working space through its own cube before compositing,
    // and the display transform runs once, after the last layer.
    gradeDocument.color_management.enabled = true;
    Document managedBaseline = gradeDocument;
    managedBaseline.sequence.tracks[0].clips[0].effects.clear();
    gradeSettings.output_path = (directory / "managed-baseline.mp4").string();
    ExportPlan managedBaselinePlan;
    Check(Exporter::BuildPlan(managedBaseline, project.string(), gradeSettings,
                              managedBaselinePlan, error) &&
              managedBaselinePlan.filter_graph.find("lut3d=file=") <
                  managedBaselinePlan.filter_graph.find("maskedmerge=") &&
              managedBaselinePlan.filter_graph.rfind("lut3d=file=") >
                  managedBaselinePlan.filter_graph.rfind("maskedmerge=") &&
              Count(managedBaselinePlan.filter_graph, "lut3d=file=") == 2 &&
              Count(managedBaselinePlan.filter_graph, "lut1d=file=") == 1 &&
              managedBaselinePlan.filter_graph.find("overlay=") ==
                  std::string::npos &&
              Exporter::Run(managedBaselinePlan, {}, nullptr, error),
          "a managed clip meets the display transform once, after "
          "composition: " +
              error);
    gradeSettings.output_path = (directory / "managed-grade.mp4").string();
    ExportPlan managedGradePlan;
    std::array<double, 3> managedBaselineMean{};
    std::array<double, 3> managedGradeMean{};
    Check(Exporter::BuildPlan(gradeDocument, project.string(), gradeSettings,
                              managedGradePlan, error) &&
              managedGradePlan.clip_grade_luts.size() == 1 &&
              !managedGradePlan.temporary_lut_path.empty() &&
              !managedGradePlan.display_shaper.cube.empty() &&
              Exporter::Run(managedGradePlan, {}, nullptr, error) &&
              !std::filesystem::exists(managedGradePlan.temporary_lut_path) &&
              !std::filesystem::exists(managedGradePlan.display_shaper.path) &&
              MeanFirstRgbFrame(managedBaselinePlan.settings.output_path, 320,
                                180, managedBaselineMean) &&
              MeanFirstRgbFrame(managedGradePlan.settings.output_path, 320, 180,
                                managedGradeMean) &&
              managedGradeMean[0] > managedBaselineMean[0] + 10.0 &&
              managedGradeMean[1] > managedBaselineMean[1] + 10.0 &&
              managedGradeMean[2] > managedBaselineMean[2] + 10.0,
          "a graded clip exports under colour management, grade visible: " +
              error);

    Document repeatedGrade = Fixture(media.string());
    repeatedGrade.sequence.tracks[0].clips[0].effects = {
        {GenerateUlid(), "color.exposure", {{"amount", {4, 1}}}}};
    repeatedGrade.sequence.tracks[1].clips[0].effects = {
        {GenerateUlid(), "color.exposure", {{"amount", {4, 1}}}}};
    gradeSettings.output_path = (directory / "repeated-grade.mp4").string();
    ExportPlan repeatedGradePlan;
    Check(Exporter::BuildPlan(repeatedGrade, project.string(), gradeSettings,
                              repeatedGradePlan, error) &&
              repeatedGradePlan.clip_grade_luts.size() == 1 &&
              Count(repeatedGradePlan.filter_graph, "lut3d=file=") == 2,
          "two clips with identical resolved grades share one derived LUT");

    Document transitionDocument = Fixture(media.string());
    DocumentClip incoming{"01K40000000000000000000008",
                          transitionDocument.sources[0].id,
                          {10, 25},
                          {5, 25},
                          {15, 25},
                          false};
    transitionDocument.sequence.tracks[0].clips.push_back(incoming);
    transitionDocument.sequence.transitions = {{
        "01K40000000000000000000009",
        transitionDocument.sequence.tracks[0].id,
        transitionDocument.sequence.tracks[0].clips[0].id,
        incoming.id,
        "cross_dissolve",
        {4, 25},
        TransitionAlignment::Center,
    }};
    ExportPlan transitionPlan;
    Check(Exporter::BuildPlan(transitionDocument, project.string(), settings,
                              transitionPlan, error) &&
              transitionPlan.filter_graph.find("fade=t=in:s=0:n=4:alpha=1") !=
                  std::string::npos &&
              transitionPlan.filter_graph.find("setpts=PTS-STARTPTS+13[") !=
                  std::string::npos,
          "export extends video handles and applies the same cross dissolve");

    Document legacyFlagOnly = Fixture(media.string());
    legacyFlagOnly.sequence.tracks.pop_back();
    legacyFlagOnly.sequence.tracks[0].clips[0].include_audio = true;
    ExportPlan silentPlan;
    Check(Exporter::BuildPlan(legacyFlagOnly, project.string(), settings,
                              silentPlan, error) &&
              silentPlan.filter_graph.find("amix=") == std::string::npos,
          "video tracks never contribute embedded audio to export");

    // PRD16 -- the H.264 delivery joined the four HEVC presets, so that the
    // window, the CLI and the catalog name one set.
    Check(Exporter::Presets().size() == 5,
          "the export UI is backed by the core preset catalog");
    CheckPresetVocabulary();
    CheckExportTaskSteps(directory);
    const ExportSettings master = Exporter::SettingsForPreset(
        ExportPresetId::HevcMaster, destination.string(), 3840, 2160,
        {30000, 1001});
    Check(master.width == 3840 && master.height == 2160 &&
              master.frame_rate.num == 30000 && master.frame_rate.den == 1001 &&
              master.encoder == ExportEncoder::HevcSoftware,
          "master preset follows source geometry and selects x265");
    const ExportSettings web = Exporter::SettingsForPreset(
        ExportPresetId::HevcWeb1080, destination.string(), 3840, 2160, {25, 1});
    Check(web.width == 1920 && web.height == 1080 &&
              web.video_bitrate == 20000000,
          "web preset fixes its delivery resolution and bitrate");

    int progressCalls = 0;
    std::atomic_bool cancel{false};
    const bool rendered = Exporter::Run(
        plan,
        [&](const ExportProgress& progress) {
            ++progressCalls;
            Check(progress.fraction >= 0.0 && progress.fraction <= 1.0,
                  "progress stays normalized");
        },
        &cancel, error);
    Check(rendered, "HEVC render succeeds: " + error);
    Check(progressCalls > 0, "render reports progress");
    Check(std::filesystem::is_regular_file(destination),
          "render is atomically committed to its destination");
    Check(!std::filesystem::exists(plan.temporary_output_path),
          "temporary output is removed after commit");

    AVFormatContext* format = nullptr;
    Check(avformat_open_input(&format, destination.c_str(), nullptr, nullptr) >=
              0,
          "exported container opens");
    if (format) {
        Check(avformat_find_stream_info(format, nullptr) >= 0,
              "exported streams can be probed");
        bool foundHevc = false;
        bool foundAudio = false;
        for (unsigned int index = 0; index < format->nb_streams; ++index) {
            const AVCodecParameters* parameters =
                format->streams[index]->codecpar;
            foundHevc = foundHevc || parameters->codec_id == AV_CODEC_ID_HEVC;
            foundAudio =
                foundAudio || parameters->codec_type == AVMEDIA_TYPE_AUDIO;
        }
        Check(foundHevc, "export contains an HEVC video stream");
        Check(foundAudio, "export contains an audio stream");
        avformat_close_input(&format);
    }

    ExportSettings invalid = settings;
    invalid.width = 319;
    Check(!Exporter::BuildPlan(document, project.string(), invalid, plan,
                               error) &&
              error.find("dimensions") != std::string::npos,
          "odd HEVC dimensions are rejected before rendering");
    document.color_management.enabled = true;
    document.color_management.input_gamut = "sony_sgamut3_cine";
    document.color_management.input_transfer = "sony_slog3";
    document.color_management.input_ycbcr_matrix = "bt709";
    document.color_management.input_range = "full";
    document.color_management.output_gamut = "rec2020";
    document.color_management.output_transfer = "hlg";
    ExportSettings colorSettings = settings;
    colorSettings.output_path = (directory / "hlg.mp4").string();
    ExportPlan colorPlan;
    Check(Exporter::BuildPlan(document, project.string(), colorSettings,
                              colorPlan, error),
          "S-Log3 to HLG export plan builds: " + error);
    Check(!colorPlan.color_lut.empty() &&
              colorPlan.filter_graph.find("lut3d=") != std::string::npos,
          "color-managed export carries its display transform LUT");
    Check(Exporter::Run(colorPlan, {}, &cancel, error),
          "S-Log3 to HLG render succeeds: " + error);
    Check(!std::filesystem::exists(colorPlan.temporary_lut_path),
          "temporary color transform is removed after rendering");
    AVFormatContext* hlgFormat = nullptr;
    Check(avformat_open_input(&hlgFormat, colorSettings.output_path.c_str(),
                              nullptr, nullptr) >= 0 &&
              avformat_find_stream_info(hlgFormat, nullptr) >= 0,
          "HLG export can be probed");
    if (hlgFormat) {
        const AVCodecParameters* video = nullptr;
        for (unsigned int index = 0; index < hlgFormat->nb_streams; ++index)
            if (hlgFormat->streams[index]->codecpar->codec_type ==
                AVMEDIA_TYPE_VIDEO)
                video = hlgFormat->streams[index]->codecpar;
        Check(video && video->color_primaries == AVCOL_PRI_BT2020 &&
                  video->color_trc == AVCOL_TRC_ARIB_STD_B67 &&
                  video->color_space == AVCOL_SPC_BT2020_NCL,
              "HEVC stream is tagged Rec.2020 HLG");
        avformat_close_input(&hlgFormat);
    }
    std::string commandOutput;
    std::string headlessProjectPath;
    Project headlessProject =
        Project::FromDocument(document, "Headless export");
    DocumentSequence emptyTimeline;
    emptyTimeline.id = GenerateUlid();
    emptyTimeline.name = "Empty alternate";
    emptyTimeline.width = document.sequence.width;
    emptyTimeline.height = document.sequence.height;
    emptyTimeline.frame_rate = document.sequence.frame_rate;
    headlessProject.timelines.push_back(emptyTimeline);
    Check(CreatePortableProject(
              (directory / "Headless.cutmachine-project").string(),
              headlessProject, headlessProjectPath, error),
          "headless export project package saves: " + error);
    Check(ExportCommand(headlessProjectPath, invalid, {}, nullptr,
                        commandOutput) == 1 &&
              commandOutput.find("\"error\":\"InvalidExport\"") !=
                  std::string::npos,
          "headless export returns a structured validation error");
    ExportSettings explicitSettings = Exporter::HevcMain10SoftwarePreset(
        (directory / "explicit-empty.mp4").string());
    explicitSettings.ffmpeg_path = FFMPEG_EXECUTABLE;
    Check(ExportCommand(headlessProjectPath, explicitSettings, {}, nullptr,
                        commandOutput, emptyTimeline.id) == 1 &&
              commandOutput.find("cannot export an empty timeline") !=
                  std::string::npos,
          "an explicit timeline is selected instead of the active one");
    Check(ExportCommand(headlessProjectPath, invalid, {}, nullptr,
                        commandOutput, GenerateUlid()) == 1 &&
              commandOutput.find("\"error\":\"UnknownSequence\"") !=
                  std::string::npos,
          "an unknown explicit export timeline is refused");

    const std::filesystem::path sequenceDestination =
        directory / "sequence-size.mp4";
    ExportSettings sequenceSettings =
        Exporter::HevcMain10SoftwarePreset(sequenceDestination.string());
    sequenceSettings.ffmpeg_path = FFMPEG_EXECUTABLE;
    Check(ExportCommand(headlessProjectPath, sequenceSettings, {}, nullptr,
                        commandOutput) == 0,
          "headless high-quality preset follows the sequence format: " +
              commandOutput);
    AVFormatContext* sequenceFormat = nullptr;
    Check(avformat_open_input(&sequenceFormat, sequenceDestination.c_str(),
                              nullptr, nullptr) >= 0 &&
              avformat_find_stream_info(sequenceFormat, nullptr) >= 0,
          "sequence-sized headless export can be probed");
    if (sequenceFormat) {
        const AVCodecParameters* video = nullptr;
        for (unsigned int index = 0; index < sequenceFormat->nb_streams;
             ++index)
            if (sequenceFormat->streams[index]->codecpar->codec_type ==
                AVMEDIA_TYPE_VIDEO)
                video = sequenceFormat->streams[index]->codecpar;
        Check(video && video->width == document.sequence.width &&
                  video->height == document.sequence.height,
              "headless export dimensions match the active sequence");
        avformat_close_input(&sequenceFormat);
    }

    // ADOBE-CLI-2026-09 -- inspect and decode real match-source deliveries
    // from both transports, including an inactive portrait NTSC timeline.
    Document delivery = Fixture(media.string());
    delivery.sequence.width = 180;
    delivery.sequence.height = 320;
    delivery.sequence.frame_rate = {30000, 1001};
    for (auto& track : delivery.sequence.tracks) {
        for (auto& clip : track.clips) {
            clip.timeline_in = {clip.timeline_in.value * 1001, 30000};
            clip.duration = {clip.duration.value * 1001, 30000};
        }
    }
    Project deliveryProject = Project::FromDocument(delivery, "H.264 delivery");
    const Ulid deliveryId = delivery.sequence.id;
    DocumentSequence inactive;
    inactive.name = "Empty active timeline";
    deliveryProject.timelines.push_back(inactive);
    deliveryProject.active_timeline_id = inactive.id;
    std::string deliveryPath;
    Check(
        CreatePortableProject((directory / "H264.cutmachine-project").string(),
                              deliveryProject, deliveryPath, error),
        "H.264 project package saves: " + error);
    const std::string deliveryBefore = deliveryProject.SaveToString();
    ExportSettings h264 =
        Exporter::H264Preset((directory / "h264-cli.mp4").string());
    h264.encoder = ExportEncoder::H264Software;
    h264.ffmpeg_path = FFMPEG_EXECUTABLE;
    Check(ExportCommand(deliveryPath, h264, {}, nullptr, commandOutput,
                        deliveryId) == 0 &&
              commandOutput.find("\"codec\":\"h264\"") != std::string::npos &&
              commandOutput.find("\"range\":\"entire\"") != std::string::npos,
          "CLI exports the entire addressed H.264 timeline: " + commandOutput);

    const char* originalPath = std::getenv("PATH");
    const std::string savedPath = (originalPath != nullptr) ? originalPath : "";
    const std::string testPath =
        std::filesystem::path(FFMPEG_EXECUTABLE).parent_path().string() + ":" +
        savedPath;
    setenv("PATH", testPath.c_str(), 1);
    const auto mcpDestination = directory / "h264-mcp.mp4";
    const std::string exportArguments =
        "{\"timeline_id\":\"" + deliveryId + "\",\"output_path\":\"" +
        mcpDestination.string() + "\",\"software\":true}";
    Check(
        CallToolCommand(deliveryPath, "export_timeline", exportArguments,
                        commandOutput, true) == 0,
        "shared CLI/MCP tool exports the inactive timeline: " + commandOutput);
    Check(CallToolCommand(deliveryPath, "export_timeline", exportArguments,
                          commandOutput, true) != 0 &&
              commandOutput.find("already exists") != std::string::npos,
          "MCP export refuses to overwrite an existing delivery");
    // VQA2 -- the Rec.709-A tag reaches the engine from the tool, and an
    // .mp4 asked for it is refused by name.
    Check(CallToolCommand(deliveryPath, "export_timeline",
                          "{\"timeline_id\":\"" + deliveryId +
                              "\",\"output_path\":\"" +
                              (directory / "h264-709a.mp4").string() +
                              "\",\"software\":true,\"sdr_tag\":\"rec709a\"}",
                          commandOutput, true) != 0 &&
              commandOutput.find("Rec.709-A needs a QuickTime .mov") !=
                  std::string::npos,
          "MCP export refuses Rec.709-A in an .mp4: " + commandOutput);
    Check(CallToolCommand(deliveryPath, "export_timeline",
                          "{\"timeline_id\":\"" + deliveryId +
                              "\",\"output_path\":\"" +
                              (directory / "h264-709a.mov").string() +
                              "\",\"software\":true,\"sdr_tag\":\"rec709a\"}",
                          commandOutput, true) == 0,
          "MCP exports a Rec.709-A .mov: " + commandOutput);
    // ITW1 -- the verification is reachable from the same catalog, scoped
    // to the addressed timeline rather than the active one.
    Check(
        CallToolCommand(deliveryPath, "verify_export",
                        "{\"timeline_id\":\"" + deliveryId + "\",\"path\":\"" +
                            mcpDestination.string() + "\"}",
                        commandOutput, true) == 0 &&
            commandOutput.find("\"exact\":true") != std::string::npos,
        "shared CLI/MCP tool verifies the export it produced: " +
            commandOutput);
    Check(
        CallToolCommand(deliveryPath, "verify_export",
                        "{\"timeline_id\":\"" + deliveryId + "\",\"path\":\"" +
                            (directory / "missing.mp4").string() + "\"}",
                        commandOutput, true) != 0 &&
            commandOutput.find("does not exist") != std::string::npos,
        "verification refuses a missing file: " + commandOutput);
    const auto liveDestination = directory / "h264-live.mp4";
    ExportSettings liveSettings = h264;
    liveSettings.output_path = liveDestination.string();
    liveSettings.width = 180;
    liveSettings.height = 320;
    liveSettings.frame_rate = {30000, 1001};
    EditLog liveLog;
    McpLiveBackend liveBackend(delivery, liveLog);
    liveBackend.SetProjectPath(deliveryPath);
    std::string exportErrorName;
    Check(liveBackend.ExportTimeline(liveSettings, commandOutput,
                                     exportErrorName, error),
          "live backend exports its current document snapshot: " + error);
    for (const auto& path : {std::filesystem::path(h264.output_path),
                             mcpDestination, liveDestination}) {
        AVFormatContext* exported = nullptr;
        Check(avformat_open_input(&exported, path.c_str(), nullptr, nullptr) >=
                      0 &&
                  avformat_find_stream_info(exported, nullptr) >= 0,
              "H.264 delivery opens and can be probed");
        bool validVideo = false;
        bool validAudio = false;
        if (exported != nullptr) {
            for (unsigned index = 0; index < exported->nb_streams; ++index) {
                const auto* stream = exported->streams[index];
                const auto* parameters = stream->codecpar;
                if (parameters->codec_type == AVMEDIA_TYPE_VIDEO) {
                    validVideo = parameters->codec_id == AV_CODEC_ID_H264 &&
                                 parameters->width == 180 &&
                                 parameters->height == 320 &&
                                 stream->avg_frame_rate.num == 30000 &&
                                 stream->avg_frame_rate.den == 1001 &&
                                 stream->nb_frames == 15;
                }
                if (parameters->codec_type == AVMEDIA_TYPE_VIDEO &&
                    !validVideo) {
                    std::cerr << "Video: codec=" << parameters->codec_id
                              << " size=" << parameters->width << 'x'
                              << parameters->height
                              << " fps=" << stream->avg_frame_rate.num << '/'
                              << stream->avg_frame_rate.den
                              << " frames=" << stream->nb_frames << '\n';
                }
                if (parameters->codec_type == AVMEDIA_TYPE_AUDIO) {
                    validAudio = parameters->codec_id == AV_CODEC_ID_AAC &&
                                 parameters->sample_rate == 48000 &&
                                 parameters->ch_layout.nb_channels == 2;
                }
            }
            avformat_close_input(&exported);
        }
        Check(validVideo && validAudio,
              "H.264 delivery preserves portrait dimensions, NTSC cadence, 15 "
              "frames and audio");
        const std::string decode =
            Quote(FFMPEG_EXECUTABLE) + " -v error -xerror -i " +
            Quote(path.string()) + " -map 0:v -map 0:a -f null -";
        Check(std::system(decode.c_str()) == 0,
              "the complete exported video and audio decode without errors");
    }
    if (originalPath != nullptr) {
        setenv("PATH", savedPath.c_str(), 1);
    } else {
        unsetenv("PATH");
    }
    Project afterDelivery;
    Check(LoadStoredProject(deliveryPath, afterDelivery, error) &&
              afterDelivery.SaveToString() == deliveryBefore &&
              liveLog.AppliedCount() == 0,
          "exports preserve the canonical project and live edit journal");
    Document offGrid = delivery;
    offGrid.sequence.tracks[0].clips[0].duration = {10011, 30000};
    ExportSettings offGridSettings = liveSettings;
    offGridSettings.output_path = (directory / "fractional.mp4").string();
    ExportPlan rejected;
    Check(!Exporter::BuildPlan(offGrid, deliveryPath, offGridSettings, rejected,
                               error) &&
              error.find("whole export frames") != std::string::npos,
          "export refuses a fractional last frame instead of rounding");
    ExportSettings unsupported = liveSettings;
    unsupported.main10 = true;
    Check(!Exporter::BuildPlan(delivery, deliveryPath, unsupported, rejected,
                               error) &&
              error.find("8-bit") != std::string::npos,
          "H.264 rejects a conflicting 10-bit profile");

    // ADOBE-CLI-2026-09 -- inspect decoded pixels, not just filter syntax:
    // literal UTF-8 captions must exist only in their half-open time range.
    Document captions;
    captions.sequence.width = 320;
    captions.sequence.height = 180;
    captions.sequence.frame_rate = {25, 1};
    captions.sequence.caption_styles = {
        {GenerateUlid(), "Sans", 18, "#ffffff", "center"}};
    DocumentClip caption;
    caption.source_in = {0, 25};
    caption.timeline_in = {5, 25};
    caption.duration = {5, 25};
    caption.include_audio = false;
    caption.caption_group_id = captions.sequence.caption_styles[0].id;
    caption.caption_text = "ÉTÉ %{pts}\nSOL '[]:\\";
    captions.sequence.tracks = {{GenerateUlid(), "caption", 0, {caption}}};
    DocumentClip hidden = caption;
    hidden.id = GenerateUlid();
    hidden.timeline_in = {0, 25};
    hidden.duration = {15, 25};
    captions.sequence.tracks.push_back(
        {GenerateUlid(), "caption", 1, {hidden}});
    captions.sequence.tracks.back().visible = false;
    ExportSettings captionSettings = liveSettings;
    captionSettings.width = 320;
    captionSettings.height = 180;
    captionSettings.frame_rate = {25, 1};
    captionSettings.output_path = (directory / "captions.mp4").string();
    ExportPlan captionPlan;
    Check(Exporter::BuildPlan(captions, project.string(), captionSettings,
                              captionPlan, error) &&
              captionPlan.caption_assets.size() == 1 &&
              !std::filesystem::exists(captionPlan.caption_assets[0].directory),
          "caption preview draws no caption picture");
    Check(Exporter::Run(captionPlan, {}, nullptr, error),
          "literal UTF-8 and filter punctuation captions encode: " + error);
    for (const auto& asset : captionPlan.caption_assets) {
        Check(!std::filesystem::exists(asset.directory),
              "successful export cleans up the caption pictures");
    }
    const auto pixelsPath = directory / "captions.rgb";
    const std::string decodePixels =
        Quote(FFMPEG_EXECUTABLE) + " -v error -xerror -i " +
        Quote(captionSettings.output_path) +
        " -map 0:v -pix_fmt rgb24 -f rawvideo -y " + Quote(pixelsPath.string());
    Check(std::system(decodePixels.c_str()) == 0,
          "caption delivery completely decodes to inspectable pixels");
    std::ifstream pixelsFile(pixelsPath, std::ios::binary);
    const std::string pixels((std::istreambuf_iterator<char>(pixelsFile)), {});
    constexpr size_t frameBytes = size_t{320} * 180 * 3;
    Check(pixels.size() == 15 * frameBytes,
          "caption export has 15 exact frames");
    if (pixels.size() == 15 * frameBytes) {
        for (size_t frame = 0; frame < 15; ++frame) {
            size_t bright = 0;
            size_t rows = 0;
            for (size_t offset = 0; offset < frameBytes; offset += 3) {
                if (static_cast<unsigned char>(
                        pixels[frame * frameBytes + offset]) > 180) {
                    ++bright;
                    rows += offset / (size_t{320} * 3);
                }
            }
            if (frame >= 5 && frame < 10) {
                Check(bright > 100 && rows / bright > 65 && rows / bright < 115,
                      "caption pixels are centered during the exact active "
                      "range");
            } else {
                Check(bright == 0,
                      "no caption pixels before, after, or from hidden tracks");
            }
        }
    }

    // PA4 -- docs/roadmap/PARITE-2026-11.md. A carton whose words are timed,
    // under a style that lights the word being said: one picture per word
    // in the plan, and in the file the lit word moves along the line on the
    // frames the words start on.
    {
        Document spoken = captions;
        CaptionStyle& lit = spoken.sequence.caption_styles[0];
        lit.font_size = 30;
        lit.word_animation = kCaptionWordAnimationHighlight;
        lit.highlight_color = "#00ff00";
        DocumentClip& carton = spoken.sequence.tracks[0].clips[0];
        carton.caption_text = "ETE SOL FIN";
        // "ETE" as the carton opens on frame 5, "SOL" on 7, "FIN" on 9.
        carton.caption_word_times = {{0, 1}, {2, 25}, {4, 25}};
        ExportSettings spokenSettings = captionSettings;
        spokenSettings.output_path =
            (directory / "captions-words.mp4").string();
        ExportPlan spokenPlan;
        const bool planned = Exporter::BuildPlan(
            spoken, project.string(), spokenSettings, spokenPlan, error);
        Check(planned && spokenPlan.caption_assets.size() == 1,
              "an animated carton plans: " + error);
        if (planned && spokenPlan.caption_assets.size() == 1) {
            const ExportCaptionAsset& asset = spokenPlan.caption_assets[0];
            const std::vector<std::vector<int32_t>> words = {{}, {1}, {2}, {3}};
            const std::vector<uint32_t> frames = {0, 0, 0, 0, 0, 1, 1, 2,
                                                  2, 3, 0, 0, 0, 0, 0};
            Check(asset.pictures.size() == 4 && asset.picture_words == words &&
                      asset.frames == frames,
                  "one picture per word started, each frame on its own: " +
                      std::to_string(asset.pictures.size()) + " pictures");
            Check(Exporter::Run(spokenPlan, {}, nullptr, error),
                  "the animated carton encodes: " + error);
            const auto wordsPath = directory / "captions-words.rgb";
            const std::string decodeWords =
                Quote(FFMPEG_EXECUTABLE) + " -v error -xerror -i " +
                Quote(spokenSettings.output_path) +
                " -map 0:v -pix_fmt rgb24 -f rawvideo -y " +
                Quote(wordsPath.string());
            Check(std::system(decodeWords.c_str()) == 0,
                  "the animated delivery decodes");
            std::ifstream wordsFile(wordsPath, std::ios::binary);
            const std::string decoded(
                (std::istreambuf_iterator<char>(wordsFile)), {});
            Check(decoded.size() == 15 * frameBytes,
                  "the animated export has 15 frames");
            if (decoded.size() == 15 * frameBytes) {
                // Where the green of the lit word sits on each frame: its
                // mean column, -1 without any.
                std::vector<double> column(15, -1.0);
                std::vector<size_t> white(15, 0);
                for (size_t frame = 0; frame < 15; ++frame) {
                    size_t green = 0;
                    size_t columns = 0;
                    for (size_t offset = 0; offset < frameBytes; offset += 3) {
                        const auto at = [&](size_t channel) {
                            return static_cast<int>(static_cast<unsigned char>(
                                decoded[frame * frameBytes + offset +
                                        channel]));
                        };
                        if (at(1) > 150 && at(1) - at(0) > 80) {
                            ++green;
                            columns += (offset / 3) % 320;
                        } else if (at(0) > 180 && at(1) > 180 && at(2) > 180) {
                            ++white[frame];
                        }
                    }
                    if (green > 20)
                        column[frame] = static_cast<double>(columns) / green;
                }
                Check(column[4] < 0 && column[10] < 0 && white[4] == 0,
                      "nothing is drawn outside the carton");
                Check(column[5] > 0 && column[6] == column[5] &&
                          column[7] > column[5] + 20 &&
                          column[8] == column[7] && column[9] > column[7] + 20,
                      "the lit word moves right on frames 7 and 9: " +
                          std::to_string(column[5]) + " " +
                          std::to_string(column[7]) + " " +
                          std::to_string(column[9]));
                Check(white[5] > 50 && white[9] > 50,
                      "the other words keep the style's white");
            }
        }
        // A style that animates nothing keeps one picture for the carton.
        spoken.sequence.caption_styles[0].word_animation.clear();
        ExportSettings stillSettings = spokenSettings;
        stillSettings.output_path = (directory / "captions-still.mp4").string();
        ExportPlan stillPlan;
        Check(Exporter::BuildPlan(spoken, project.string(), stillSettings,
                                  stillPlan, error) &&
                  stillPlan.caption_assets.size() == 1 &&
                  stillPlan.caption_assets[0].pictures.size() == 2 &&
                  stillPlan.caption_assets[0].picture_words[1] ==
                      std::vector<int32_t>{-1},
              "without an animation the carton is one picture, drawn whole");
    }

    // PA8 -- docs/roadmap/PARITE-2026-11.md. A masked clip over another:
    // inside the shape the clip, outside it the clip below, and on a
    // feathered edge the two mixed -- in the delivered signal without
    // colour management, in linear light with it, which is brighter.
    {
        const auto redPath = directory / "mask-red.mp4";
        const auto whitePath = directory / "mask-white.mp4";
        const auto generate = [&](const char* colour,
                                  const std::filesystem::path& path) {
            const std::string command =
                Quote(FFMPEG_EXECUTABLE) +
                " -hide_banner -loglevel error -f lavfi -i 'color=c=" + colour +
                ":size=320x180:rate=25:duration=1' -c:v libx264 -pix_fmt "
                "yuv420p -y " +
                Quote(path.string());
            return std::system(command.c_str()) == 0;
        };
        Check(generate("red", redPath) && generate("white", whitePath),
              "the two colour sources are generated");
        Document layered;
        layered.sequence.width = 320;
        layered.sequence.height = 180;
        layered.sequence.frame_rate = {25, 1};
        layered.sources = {
            {"01K4F7W100000000000000000R", redPath.string(), {25, 1}, {25, 25}},
            {"01K4F7W100000000000000000W",
             whitePath.string(),
             {25, 1},
             {25, 25}}};
        for (const DocumentSource& source : layered.sources) {
            LibraryMedia media;
            media.id = source.id;
            media.path = source.path;
            media.filename = std::filesystem::path(source.path).filename();
            media.codec = "h264";
            media.orientation = "landscape";
            media.has_video = true;
            media.width = 320;
            media.height = 180;
            media.rate = {25, 1};
            media.duration = {25, 25};
            media.metadata_complete = true;
            layered.library.push_back(media);
        }
        DocumentClip below;
        below.source_id = layered.sources[0].id;
        below.source_in = {0, 25};
        below.duration = {10, 25};
        below.timeline_in = {0, 25};
        below.include_audio = false;
        DocumentClip above = below;
        above.id = GenerateUlid();
        above.source_id = layered.sources[1].id;
        layered.sequence.tracks = {{GenerateUlid(), "video", 0, {below}},
                                   {GenerateUlid(), "video", 1, {above}}};
        // The pixel at (x, y) of frame 4 of an export of `document`.
        struct Rgb {
            int r = 0;
            int g = 0;
            int b = 0;
        };
        int exported = 0;
        const auto render = [&](const Document& document, bool managed,
                                std::string& pixels) {
            Document subject = document;
            subject.color_management.enabled = managed;
            ExportSettings maskSettings = captionSettings;
            maskSettings.output_path =
                (directory / ("mask-" + std::to_string(++exported) + ".mp4"))
                    .string();
            ExportPlan plan;
            if (!Exporter::BuildPlan(subject, project.string(), maskSettings,
                                     plan, error) ||
                !Exporter::Run(plan, {}, nullptr, error))
                return false;
            const auto raw = directory / "mask.rgb";
            const std::string decode =
                Quote(FFMPEG_EXECUTABLE) + " -v error -xerror -i " +
                Quote(maskSettings.output_path) +
                " -map 0:v -vf select='eq(n\\,4)' -frames:v 1 -pix_fmt rgb24 "
                "-f rawvideo -y " +
                Quote(raw.string());
            if (std::system(decode.c_str()) != 0) return false;
            std::ifstream file(raw, std::ios::binary);
            pixels.assign((std::istreambuf_iterator<char>(file)), {});
            return pixels.size() == size_t{320} * 180 * 3;
        };
        const auto at = [](const std::string& pixels, int x, int y) {
            const size_t offset = (static_cast<size_t>(y) * 320 + x) * 3;
            return Rgb{static_cast<unsigned char>(pixels[offset]),
                       static_cast<unsigned char>(pixels[offset + 1]),
                       static_cast<unsigned char>(pixels[offset + 2])};
        };
        const auto white = [](const Rgb& pixel) {
            return pixel.r > 215 && pixel.g > 215 && pixel.b > 215;
        };
        const auto red = [](const Rgb& pixel) {
            return pixel.r > 170 && pixel.g < 60 && pixel.b < 60;
        };
        std::string pixels;
        Check(render(layered, false, pixels) && white(at(pixels, 160, 90)) &&
                  white(at(pixels, 20, 20)),
              "without a mask the clip above covers the frame: " + error);

        // The middle half kept, hard-edged.
        Document halved = layered;
        ClipMask& mask = halved.sequence.tracks[1].clips[0].mask;
        mask.shape = kClipMaskRectangle;
        mask.width = {1, 2};
        Check(render(halved, false, pixels) && white(at(pixels, 160, 90)) &&
                  white(at(pixels, 90, 170)) && red(at(pixels, 40, 90)) &&
                  red(at(pixels, 300, 20)),
              "a rectangle keeps the middle half and shows the clip below "
              "beside it: " +
                  error);
        mask.inverted = true;
        Check(render(halved, false, pixels) && red(at(pixels, 160, 90)) &&
                  white(at(pixels, 40, 90)) && white(at(pixels, 300, 20)),
              "inverted, it cuts the middle half out: " + error);

        // A feathered ellipse: whole at its centre, gone at its edge, half
        // of each a quarter of the way in (x = 100: 3/4 of the half-width
        // from the centre, on a feather of 1/2).
        Document soft = layered;
        ClipMask& round = soft.sequence.tracks[1].clips[0].mask;
        round.shape = kClipMaskEllipse;
        round.width = {1, 2};
        round.feather = {1, 2};
        Check(render(soft, false, pixels),
              "a feathered ellipse exports: " + error);
        const Rgb unmanagedMix = at(pixels, 100, 90);
        Check(white(at(pixels, 160, 90)) && red(at(pixels, 60, 90)) &&
                  red(at(pixels, 160, 4)) && unmanagedMix.r > 200 &&
                  unmanagedMix.g > 95 && unmanagedMix.g < 160,
              "its edge mixes the two clips half and half in the delivered "
              "signal: green " +
                  std::to_string(unmanagedMix.g));
        std::string managedPixels;
        Check(render(soft, true, managedPixels),
              "and under colour management: " + error);
        if (managedPixels.size() == pixels.size()) {
            const Rgb managedMix = at(managedPixels, 100, 90);
            const Rgb managedCentre = at(managedPixels, 160, 90);
            const Rgb managedOutside = at(managedPixels, 60, 90);
            Check(managedCentre.g > 200 &&
                      managedOutside.g < managedOutside.r / 2 &&
                      managedMix.g > unmanagedMix.g + 20 &&
                      managedMix.g < managedCentre.g,
                  "there the same edge is mixed in linear light, brighter "
                  "than in the delivered signal: green " +
                      std::to_string(managedMix.g) + " against " +
                      std::to_string(unmanagedMix.g));
        }
    }

    // ITW4 -- a white carton over a white shirt was illegible. The outline
    // makes it readable in the delivered pixels, and a custom vertical
    // position centres the block on that fraction of the height.
    {
        const auto white = directory / "white.mp4";
        const std::string generateWhite =
            Quote(FFMPEG_EXECUTABLE) +
            " -hide_banner -loglevel error -f lavfi -i "
            "'color=c=white:size=320x180:rate=25:duration=1' -c:v libx264 "
            "-pix_fmt yuv420p -y " +
            Quote(white.string());
        Check(std::system(generateWhite.c_str()) == 0,
              "white source is generated");
        Document shirt;
        shirt.sequence.width = 320;
        shirt.sequence.height = 180;
        shirt.sequence.frame_rate = {25, 1};
        shirt.sources = {
            {"01K4F7W100000000000000000B", white.string(), {25, 1}, {25, 25}}};
        DocumentClip background;
        background.source_id = shirt.sources[0].id;
        background.source_in = {0, 25};
        background.duration = {15, 25};
        background.timeline_in = {0, 25};
        CaptionStyle plain;
        plain.font_size = 28;
        plain.color = "#ffffff";
        CaptionStyle outlined = plain;
        outlined.id = GenerateUlid();
        outlined.outline_width = 3;
        outlined.outline_color = "#000000";
        outlined.vertical_position = {3, 4};
        DocumentClip carton;
        carton.source_in = {0, 25};
        carton.timeline_in = {0, 25};
        carton.duration = {15, 25};
        carton.include_audio = false;
        carton.caption_text = "ÉTÉ";
        const auto darkRows = [&](const CaptionStyle& style,
                                  const std::string& name, size_t& dark,
                                  size_t& meanRow, std::string& graph) {
            Document document = shirt;
            document.sequence.caption_styles = {style};
            DocumentClip clip = carton;
            clip.caption_group_id = style.id;
            document.sequence.tracks = {
                {GenerateUlid(), "video", 0, {background}},
                {GenerateUlid(), "caption", 1, {clip}}};
            ExportSettings settings = captionSettings;
            settings.output_path = (directory / (name + ".mp4")).string();
            ExportPlan plan;
            std::string failure;
            if (!Exporter::BuildPlan(document, project.string(), settings, plan,
                                     failure) ||
                !Exporter::Run(plan, {}, nullptr, failure)) {
                Check(false, name + " export: " + failure);
                return false;
            }
            graph = plan.filter_graph;
            const auto raw = directory / (name + ".rgb");
            const std::string decode =
                Quote(FFMPEG_EXECUTABLE) + " -v error -i " +
                Quote(settings.output_path) +
                " -map 0:v -frames:v 1 -pix_fmt rgb24 -f rawvideo -y " +
                Quote(raw.string());
            if (std::system(decode.c_str()) != 0) return false;
            std::ifstream input(raw, std::ios::binary);
            const std::string frame((std::istreambuf_iterator<char>(input)),
                                    {});
            dark = 0;
            size_t rows = 0;
            for (size_t offset = 0; offset + 2 < frame.size(); offset += 3) {
                if (static_cast<unsigned char>(frame[offset]) < 60) {
                    ++dark;
                    rows += offset / (size_t{320} * 3);
                }
            }
            meanRow = dark == 0 ? 0 : rows / dark;
            return true;
        };
        size_t plainDark = 0;
        size_t outlinedDark = 0;
        size_t plainRow = 0;
        size_t outlinedRow = 0;
        std::string plainGraph;
        std::string outlinedGraph;
        Check(
            darkRows(plain, "carton-plain", plainDark, plainRow, plainGraph) &&
                plainDark < 20,
            "an undecorated white carton leaves a white frame white");
        Check(darkRows(outlined, "carton-outlined", outlinedDark, outlinedRow,
                       outlinedGraph) &&
                  outlinedDark > 150 && outlinedRow > 120 && outlinedRow < 150,
              "the outline draws the carton, centred at three quarters of "
              "the height: " +
                  std::to_string(outlinedDark) +
                  " dark pixels "
                  "around row " +
                  std::to_string(outlinedRow));
        // LG3 -- the cartons are one layer of pictures the caption renderer
        // drew (caption_render_tests checks their pixels); no drawtext.
        Check(plainGraph.find("drawtext") == std::string::npos &&
                  outlinedGraph.find("drawtext") == std::string::npos &&
                  outlinedGraph.find("[captionbase][captions]overlay=") !=
                      std::string::npos,
              "the cartons are laid on as a layer, not drawn by drawtext");
        CaptionStyle boxed = plain;
        boxed.background_color = "#000000@0.6";
        boxed.background_padding = 8;
        boxed.shadow_offset = 2;
        Document boxedDocument = shirt;
        boxedDocument.sequence.caption_styles = {boxed};
        DocumentClip boxedClip = carton;
        boxedClip.caption_group_id = boxed.id;
        boxedDocument.sequence.tracks = {
            {GenerateUlid(), "caption", 0, {boxedClip}}};
        ExportPlan boxedPlan;
        ExportSettings boxedSettings = captionSettings;
        boxedSettings.output_path = (directory / "carton-boxed.mp4").string();
        Check(
            Exporter::BuildPlan(boxedDocument, project.string(), boxedSettings,
                                boxedPlan, error) &&
                boxedPlan.caption_assets.size() == 1 &&
                boxedPlan.caption_assets[0].cartons.size() == 1 &&
                boxedPlan.caption_assets[0].cartons[0].style.background_color ==
                    "#000000@0.6" &&
                boxedPlan.caption_assets[0]
                        .cartons[0]
                        .style.background_padding == 8 &&
                boxedPlan.caption_assets[0].cartons[0].style.shadow_offset == 2,
            "a background box and a shadow reach the caption renderer: " +
                error);
        boxedDocument.sequence.caption_styles[0].background_color = "red:x";
        Check(!Exporter::BuildPlan(boxedDocument, project.string(),
                                   boxedSettings, boxedPlan, error),
              "a decoration color that cannot be read is refused");
    }

    // All paths and templates are preflighted before any batch export starts.
    Project batchProject = Project::FromDocument(captions, "Date variants");
    // The operation engine owns identity remapping for the second template.
    ProjectEditLog batchLog;
    EditError batchError = EditError::None;
    CloneProjectTimelineOperation templateClone;
    templateClone.source_timeline_id = captions.sequence.id;
    templateClone.name = "Second format";
    Check(batchLog.Apply(batchProject, templateClone, batchError, error),
          "second format template clones through the engine: " + error);
    const Ulid secondTemplate = batchProject.timelines.back().id;
    batchProject.timelines.back().width = 180;
    batchProject.timelines.back().height = 320;
    std::string batchPath;
    Check(
        CreatePortableProject((directory / "Batch.cutmachine-project").string(),
                              batchProject, batchPath, error),
        "variant project package saves: " + error);
    const std::string batchBefore = batchProject.SaveToString();
    setenv("PATH", testPath.c_str(), 1);
    using mcp_json::Value;
    Value arguments = Value::MakeObject();
    Value templates = Value::MakeArray();
    for (const auto& id : {captions.sequence.id, secondTemplate}) {
        Value item = Value::MakeObject();
        item.Set("timeline_id", Value::MakeString(id));
        item.Set("suffix",
                 Value::MakeString(id == secondTemplate ? "_9x16" : "_16x9"));
        item.Set("clip_id",
                 Value::MakeString(
                     batchProject.FindTimeline(id)->tracks[0].clips[0].id));
        templates.Push(std::move(item));
    }
    Value values = Value::MakeArray();
    for (const auto& date : {"03/02/2031", "27/05/2031"}) {
        Value item = Value::MakeObject();
        item.Set("name", Value::MakeString(date));
        item.Set("text", Value::MakeString(date));
        values.Push(std::move(item));
    }
    arguments.Set("templates", std::move(templates));
    arguments.Set("variants", std::move(values));
    arguments.Set("find", Value::MakeString("ÉTÉ %{pts}"));
    arguments.Set("preview", Value::MakeBool(true));
    Check(CallToolCommand(batchPath, "create_timeline_variants",
                          arguments.Dump(), commandOutput, true) == 0,
          "CLI/MCP previews dates x formats: " + commandOutput);
    Project previewProject;
    Check(LoadStoredProject(batchPath, previewProject, error) &&
              previewProject.SaveToString() == batchBefore,
          "variant preview leaves the whole package document untouched");
    arguments.Set("preview", Value::MakeBool(false));
    Check(CallToolCommand(batchPath, "create_timeline_variants",
                          arguments.Dump(), commandOutput, true) == 0,
          "CLI/MCP creates all four date variants: " + commandOutput);
    Project created;
    Check(LoadStoredProject(batchPath, created, error) &&
              created.timelines.size() == 6,
          "all variant timelines persist in the project package");
    const std::string batchAfter = created.SaveToString();
    Check(UndoProjectOperationCommand(batchPath, commandOutput) == 0 &&
              LoadStoredProject(batchPath, previewProject, error) &&
              previewProject.SaveToString() == batchBefore,
          "one CLI project undo restores exact pre-variant bytes");
    Check(RedoProjectOperationCommand(batchPath, commandOutput) == 0 &&
              LoadStoredProject(batchPath, created, error) &&
              created.SaveToString() == batchAfter,
          "CLI project redo restores every generated variant ID");
    Value exportBatch = Value::MakeObject();
    Value outputs = Value::MakeArray();
    std::vector<std::filesystem::path> batchDestinations;
    for (size_t index = 2; index < created.timelines.size(); ++index) {
        const auto path =
            directory / ("date-" + std::to_string(index) + ".mp4");
        Value output = Value::MakeObject();
        output.Set("timeline_id",
                   Value::MakeString(created.timelines[index].id));
        output.Set("output_path", Value::MakeString(path.string()));
        outputs.Push(std::move(output));
        batchDestinations.push_back(path);
    }
    exportBatch.Set("outputs", outputs);
    exportBatch.Set("software", Value::MakeBool(true));
    exportBatch.Set("preview", Value::MakeBool(true));
    Check(CallToolCommand(batchPath, "export_timelines", exportBatch.Dump(),
                          commandOutput, true) == 0,
          "batch export preview validates every exact timeline format");
    Check(!std::filesystem::exists(batchDestinations[0]),
          "batch export preview writes no deliveries");
    exportBatch.Set("preview", Value::MakeBool(false));
    // A missing final template must refuse before even the first file exists.
    Value invalidOutputs = outputs;
    Value invalidOutput = Value::MakeObject();
    invalidOutput.Set("timeline_id", Value::MakeString(GenerateUlid()));
    invalidOutput.Set("output_path",
                      Value::MakeString((directory / "unknown.mp4").string()));
    invalidOutputs.Push(std::move(invalidOutput));
    exportBatch.Set("outputs", invalidOutputs);
    Check(CallToolCommand(batchPath, "export_timelines", exportBatch.Dump(),
                          commandOutput, true) != 0 &&
              commandOutput.find("UnknownSequence") != std::string::npos &&
              !std::filesystem::exists(batchDestinations[0]),
          "invalid final item refuses before the first batch render");
    exportBatch.Set("outputs", outputs);
    Check(CallToolCommand(batchPath, "export_timelines", exportBatch.Dump(),
                          commandOutput, true) == 0 &&
              commandOutput.find("\"completed\":4") != std::string::npos,
          "CLI/MCP exports all four entire timelines: " + commandOutput);
    for (const auto& path : batchDestinations) {
        const std::string decode =
            Quote(FFMPEG_EXECUTABLE) + " -v error -xerror -i " +
            Quote(path.string()) + " -map 0:v -map 0:a -f null -";
        Check(std::system(decode.c_str()) == 0,
              "each dates x formats delivery fully decodes");
    }
    Check(LoadStoredProject(batchPath, created, error) &&
              created.SaveToString() == batchAfter,
          "batch exports preserve the canonical project");

    Document liveDocument = batchProject.MakeActiveDocument();
    EditLog liveVariantEdits;
    ProjectEditLog liveVariantProjectEdits;
    McpLiveBackend liveVariants(
        liveDocument, liveVariantEdits, nullptr,
        [&](ProjectOperation operation, std::vector<EntryMotif> motifs,
            std::string& result, std::string& errorName, std::string& detail) {
            EditError code = EditError::None;
            if (!liveVariantProjectEdits.Apply(batchProject,
                                               std::move(operation), code,
                                               detail, std::move(motifs))) {
                errorName = EditErrorName(code);
                return false;
            }
            result = "{\"ok\":true}";
            return true;
        });
    liveVariants.SetProjectSnapshotCallback(
        [&](Project& snapshot, std::string&) {
            snapshot = batchProject;
            return true;
        });
    liveVariants.SetProjectPath(batchPath);
    const McpToolRegistry registry;
    const auto liveCreated =
        registry.Call(liveVariants, "create_timeline_variants", arguments);
    Check(liveCreated.ok && batchProject.timelines.size() == 6 &&
              liveVariantProjectEdits.AppliedCount() == 1 &&
              liveVariantEdits.AppliedCount() == 0,
          "live CLI/MCP dispatch shares the atomic project variant operation");
    std::vector<std::pair<Ulid, ExportSettings>> liveItems;
    for (size_t index = 2; index < batchProject.timelines.size(); ++index) {
        ExportSettings item = captionSettings;
        // PRD16 -- zero asks the batch for each timeline's exact format, as
        // the catalog does for a codec named alone; a size given is now the
        // item's own (a preset's), and two of these timelines are portrait.
        item.width = 0;
        item.height = 0;
        item.frame_rate = {0, 1};
        item.output_path =
            (directory / ("live-date-" + std::to_string(index) + ".mp4"))
                .string();
        liveItems.emplace_back(batchProject.timelines[index].id, item);
    }
    Check(liveVariants.ExportProjectTimelines(liveItems, false, commandOutput,
                                              exportErrorName, error) &&
              commandOutput.find("\"completed\":4") != std::string::npos,
          "live backend batch exports every inactive variant: " + error);

    ExportSettings first = captionSettings;
    first.output_path = (directory / "partial-first.mp4").string();
    ExportSettings second = first;
    second.output_path = (directory / "partial-second.mp4").string();
    second.ffmpeg_path = (directory / "missing-ffmpeg").string();
    Check(
        !Exporter::ExportBatch(
            batchProject, batchPath,
            {{captions.sequence.id, first}, {secondTemplate, second}}, false,
            commandOutput, exportErrorName, error) &&
            exportErrorName == "ExportFailed" &&
            commandOutput.find("\"completed\":1") != std::string::npos &&
            std::filesystem::exists(first.output_path) &&
            !std::filesystem::exists(second.output_path),
        "failed later encoder retains the completed file and a partial report");
    first.output_path = (directory / "duplicate-output.mp4").string();
    second = first;
    Check(!Exporter::ExportBatch(
              batchProject, batchPath,
              {{captions.sequence.id, first}, {secondTemplate, second}}, false,
              commandOutput, exportErrorName, error) &&
              !std::filesystem::exists(first.output_path),
          "duplicate destinations are refused before any batch output");
    if (originalPath != nullptr) {
        setenv("PATH", savedPath.c_str(), 1);
    } else {
        unsetenv("PATH");
    }

    // R6 -- docs/roadmap/REEL-2026-09.md. The sequence look: one .cube,
    // copied into the package under its digest, read at export from that
    // copy. What is checked here is the half the parity test cannot see --
    // the package, the digest and the refusals -- plus where the filter
    // lands in the managed graph. That the file and the monitor agree on
    // the picture is cutmachine_monitor_export_parity_tests.
    {
        const std::filesystem::path lookRoot = directory / "look-package";
        std::filesystem::create_directories(lookRoot);
        std::filesystem::copy_file(media, lookRoot / "source.mp4");
        const std::filesystem::path lookProject = lookRoot / "project.json";

        // A visible, monotone look: every channel lifted towards white by a
        // fifth. Written here rather than committed -- a real look belongs
        // to its author (guard rail 7) -- and small enough to read.
        const std::filesystem::path lookSource = lookRoot / "author-look.cube";
        {
            std::ofstream cube(lookSource, std::ios::binary | std::ios::trunc);
            cube << "TITLE \"lift\"\nLUT_3D_SIZE 2\n";
            for (int blue = 0; blue < 2; ++blue)
                for (int green = 0; green < 2; ++green)
                    for (int red = 0; red < 2; ++red)
                        cube << (0.2 + 0.8 * red) << ' ' << (0.2 + 0.8 * green)
                             << ' ' << (0.2 + 0.8 * blue) << '\n';
        }

        Document lookDocument;
        lookDocument.sequence.id = GenerateUlid();
        lookDocument.sequence.width = 320;
        lookDocument.sequence.height = 180;
        lookDocument.sequence.frame_rate = {25, 1};
        // A relative path, as a collected package holds: nothing in the
        // document names the machine it was written on.
        lookDocument.sources = {
            {GenerateUlid(), "source.mp4", {25, 1}, {25, 25}}};
        DocumentClip lookClip;
        lookClip.id = GenerateUlid();
        lookClip.source_id = lookDocument.sources[0].id;
        lookClip.source_in = {0, 25};
        lookClip.duration = {25, 25};
        lookClip.timeline_in = {0, 25};
        lookClip.include_audio = false;
        lookDocument.sequence.tracks = {
            {GenerateUlid(), "video", 0, {lookClip}}};

        SequenceLut installed;
        Check(
            ImportLookLutIntoPackage(lookProject.string(), lookSource.string(),
                                     "", {}, installed, error),
            "a .cube is imported into the package: " + error);
        Check(std::filesystem::exists(lookRoot /
                                      LookLutPackagePath(installed.digest)),
              "the imported LUT is stored under its digest");
        Check(installed.name == "author-look.cube",
              "the imported LUT keeps the name it was imported under");

        ExportSettings lookSettings =
            Exporter::H264Preset((lookRoot / "without-look.mp4").string());
        lookSettings.encoder = ExportEncoder::H264Software;
        lookSettings.width = 320;
        lookSettings.height = 180;
        lookSettings.frame_rate = {25, 1};
        lookSettings.ffmpeg_path = FFMPEG_EXECUTABLE;
        ExportPlan withoutLook;
        Check(Exporter::BuildPlan(lookDocument, lookProject.string(),
                                  lookSettings, withoutLook, error) &&
                  Exporter::Run(withoutLook, {}, nullptr, error),
              "the baseline without a look renders: " + error);

        lookDocument.sequence.look_lut = installed;
        Check(lookDocument.Validate(error),
              "a document carrying a look is valid: " + error);
        lookSettings.output_path = (lookRoot / "with-look.mp4").string();
        ExportPlan withLook;
        Check(Exporter::BuildPlan(lookDocument, lookProject.string(),
                                  lookSettings, withLook, error) &&
                  Exporter::Run(withLook, {}, nullptr, error),
              "the look renders: " + error);
        Check(withLook.filter_graph.find(
                  LookLutPackagePath(installed.digest)) != std::string::npos,
              "the export reads the package's own copy of the LUT");
        std::array<double, 3> plainMean{};
        std::array<double, 3> lookedMean{};
        Check(MeanFirstRgbFrame(withoutLook.settings.output_path, 320, 180,
                                plainMean) &&
                  MeanFirstRgbFrame(withLook.settings.output_path, 320, 180,
                                    lookedMean),
              "both renders are read back");
        Check(lookedMean[0] > plainMean[0] + 8.0 &&
                  lookedMean[1] > plainMean[1] + 8.0 &&
                  lookedMean[2] > plainMean[2] + 8.0,
              "the look reaches the delivered file, and in the direction it "
              "is written");

        // The ticket's second criterion: the same project, opened from a
        // copy of the package at another path, delivers the same picture.
        // Nothing but the package moves -- the document is the same bytes,
        // and it names the LUT by digest, not by path.
        const std::filesystem::path moved = directory / "look-package-moved";
        std::filesystem::create_directories(moved);
        std::filesystem::copy(
            lookRoot, moved,
            std::filesystem::copy_options::recursive |
                std::filesystem::copy_options::overwrite_existing);
        ExportSettings movedSettings = lookSettings;
        movedSettings.output_path = (moved / "from-copy.mp4").string();
        ExportPlan movedPlan;
        Check(
            Exporter::BuildPlan(lookDocument, (moved / "project.json").string(),
                                movedSettings, movedPlan, error) &&
                Exporter::Run(movedPlan, {}, nullptr, error),
            "the copied package exports: " + error);
        std::array<double, 3> movedMean{};
        Check(MeanFirstRgbFrame(movedPlan.settings.output_path, 320, 180,
                                movedMean),
              "the copy's render is read back");
        Check(movedMean == lookedMean,
              "a package copied to another path delivers another picture");

        // A LUT that is gone, and one whose bytes changed under the same
        // name: refused by name rather than exported without the look.
        const std::filesystem::path stored =
            moved / LookLutPackagePath(installed.digest);
        {
            std::ofstream tampered(stored, std::ios::binary | std::ios::trunc);
            tampered << "TITLE \"lift\"\nLUT_3D_SIZE 2\n";
            for (int corner = 0; corner < 8; ++corner)
                tampered << "0.5 0.5 0.5\n";
        }
        ExportPlan refused;
        ExportSettings refusedSettings = movedSettings;
        refusedSettings.output_path = (moved / "tampered.mp4").string();
        Check(!Exporter::BuildPlan(lookDocument,
                                   (moved / "project.json").string(),
                                   refusedSettings, refused, error) &&
                  error.find("digest") != std::string::npos,
              "a LUT whose bytes changed is refused by name: " + error);
        std::filesystem::remove(stored);
        refusedSettings.output_path = (moved / "absent.mp4").string();
        Check(!Exporter::BuildPlan(lookDocument,
                                   (moved / "project.json").string(),
                                   refusedSettings, refused, error) &&
                  error.find("missing") != std::string::npos,
              "a LUT missing from the package is refused by name: " + error);

        // Managed: the look sits between the ACEScct shaper and the display
        // cube. The order is asserted on the graph because it is the whole
        // ticket -- "after each clip's grade, before the output transform".
        Document managedLook = lookDocument;
        managedLook.color_management.enabled = true;
        managedLook.color_management.input_gamut = "sony_sgamut3_cine";
        managedLook.color_management.input_transfer = "sony_slog3";
        managedLook.sequence.look_lut.space = "acescct";
        Check(managedLook.Validate(error),
              "a declared look is valid under colour management: " + error);
        ExportSettings managedSettings = lookSettings;
        managedSettings.output_path = (lookRoot / "managed-look.mp4").string();
        ExportPlan managedPlan;
        Check(Exporter::BuildPlan(managedLook, lookProject.string(),
                                  managedSettings, managedPlan, error),
              "a managed look plans: " + error);
        const std::string& managedGraph = managedPlan.filter_graph;
        const size_t shaperAt =
            managedGraph.find(managedPlan.display_shaper.path);
        const size_t lookAt =
            managedGraph.find(LookLutPackagePath(installed.digest));
        const size_t displayAt =
            managedGraph.find(managedPlan.temporary_lut_path);
        Check(shaperAt != std::string::npos && lookAt != std::string::npos &&
                  displayAt != std::string::npos && shaperAt < lookAt &&
                  lookAt < displayAt,
              "the managed look is not between the ACEScct shaper and the "
              "display transform");

        // Guard rail 4: under colour management, a look whose space nobody
        // declared is refused rather than laid at a plausible stage.
        Document undeclared = managedLook;
        undeclared.sequence.look_lut.space.clear();
        Check(!undeclared.Validate(error),
              "an undeclared look is valid under colour management");
        ExportPlan undeclaredPlan;
        Check(!Exporter::BuildPlan(undeclared, lookProject.string(),
                                   managedSettings, undeclaredPlan, error) &&
                  error.find("declare") != std::string::npos,
              "the export accepts an undeclared look: " + error);
    }

    std::filesystem::remove_all(directory);
    if (failures != 0) {
        std::cerr << failures << " assertion(s) failed\n";
        return 1;
    }
    std::cout << "All export tests passed\n";
    return 0;
}
