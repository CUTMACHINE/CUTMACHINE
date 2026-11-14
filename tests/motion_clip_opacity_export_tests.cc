// R2b -- docs/roadmap/REEL-2026-09.md. A fade carried by a motion clip's own
// opacity keys, exported and read back. Relevé on 22 October 2026 on
// a delivered film: an end card fading to black over its last 28 frames was
// accepted by set_clip_animation, played by the monitor, counted as a final
// fade by check_timeline, and refused by the export at its first frame.
//
// Portable on purpose: the motion renderer is a seam (MotionFrameRenderer.h),
// and what this suite asks of it is a plain white card, which a stand-in
// draws without Core Text. What is under test is the export graph, which
// FFmpeg runs here exactly as it does on a Mac: the composition's own
// rendering is motion_clip_export_tests' business.

#include "ClipAnimation.h"
#include "Document.h"
#include "Export.h"
#include "ExportVerification.h"
#include "Json.h"
#include "MotionFrameRenderer.h"
#include "Ulid.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
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

std::string Quote(const std::string& value) {
    std::string quoted = "'";
    for (const char character : value) {
        if (character == '\'') {
            quoted += "'\\''";
        } else {
            quoted += character;
        }
    }
    return quoted + "'";
}

constexpr int kWidth = 64;
constexpr int kHeight = 36;
constexpr int64_t kFrames = 40;
// The fade of the relevé: full strength until frame 12, then down to zero
// on the clip's last frame, 28 frames later.
constexpr int64_t kFadeFrom = 12;
constexpr int64_t kFadeTo = kFrames - 1;
const Ulid kComposition = "01K9R2B0000000000000000010";
const Ulid kClip = "01K9R2B0000000000000000020";

const char kDefinition[] =
    R"({"width":64,"height":36,"fps":25,"duration":60,)"
    R"("root":{"type":"fill","uid":"01K9R2B0000000000000000011",)"
    R"("style":{"backgroundColor":"#FFFFFF"}}})";

// What the stand-in renderer hands back for every frame: an opaque white
// PNG of the composition's size, written once by FFmpeg.
std::string whitePng;

bool WhiteCard(const MotionFrameRequest&, MotionFrameImage& image,
               MotionError& error) {
    if (whitePng.empty()) {
        error.code = "NoCard";
        error.message = "the stand-in card was not written";
        return false;
    }
    image.png = whitePng;
    image.width = kWidth;
    image.height = kHeight;
    return true;
}

bool WriteWhitePng(const std::filesystem::path& path) {
    const std::string command =
        Quote(FFMPEG_EXECUTABLE) +
        " -hide_banner -loglevel error -f lavfi -i color=c=white:s=" +
        std::to_string(kWidth) + 'x' + std::to_string(kHeight) +
        " -frames:v 1 -pix_fmt rgba -y " + Quote(path.string());
    if (std::system(command.c_str()) != 0) return false;
    std::ifstream input(path, std::ios::binary);
    whitePng.assign(std::istreambuf_iterator<char>(input),
                    std::istreambuf_iterator<char>());
    return !whitePng.empty();
}

// The mean luma of each frame of a file, full range, 0 to 1.
bool ReadMeanLuma(const std::string& path, std::vector<double>& means) {
    const auto raw = std::filesystem::temp_directory_path() /
                     (GenerateUlid() + "-r2b-luma.raw");
    const std::string decode =
        Quote(FFMPEG_EXECUTABLE) + " -hide_banner -loglevel error -i " +
        Quote(path) +
        " -map 0:v:0 -vf scale=out_range=full,format=gray16le "
        "-f rawvideo -pix_fmt gray16le -y " +
        Quote(raw.string());
    if (std::system(decode.c_str()) != 0) {
        std::filesystem::remove(raw);
        return false;
    }
    std::ifstream input(raw, std::ios::binary);
    const std::string bytes{std::istreambuf_iterator<char>(input),
                            std::istreambuf_iterator<char>()};
    std::filesystem::remove(raw);
    const size_t frameBytes = static_cast<size_t>(kWidth) * kHeight * 2;
    if (bytes.empty() || bytes.size() % frameBytes != 0) return false;
    means.clear();
    for (size_t offset = 0; offset < bytes.size(); offset += frameBytes) {
        double sum = 0.0;
        for (size_t at = offset; at < offset + frameBytes; at += 2) {
            const auto low = static_cast<unsigned char>(bytes[at]);
            const auto high = static_cast<unsigned char>(bytes[at + 1]);
            sum += static_cast<double>(low | (high << 8)) / 65535.0;
        }
        means.push_back(sum / (static_cast<double>(kWidth) * kHeight));
    }
    return true;
}

ClipKeyframe Key(const char* id, int64_t frame, int32_t value) {
    ClipKeyframe key;
    key.id = id;
    key.time = RationalTime(frame, 25);
    key.value = {value, 1};
    return key;
}

// The end card alone on V1, over the composite's black.
Document EndCard(bool managed) {
    Document document;
    document.sequence.id = "01K9R2B0000000000000000001";
    document.sequence.name = "R2b";
    document.sequence.width = kWidth;
    document.sequence.height = kHeight;
    document.sequence.frame_rate = {25, 1};
    document.color_management.enabled = managed;
    std::string error;
    mcp_json::Value definition;
    if (!mcp_json::Value::Parse(kDefinition, definition, error)) {
        throw std::runtime_error("composition JSON: " + error);
    }
    document.compositions = {{kComposition, "Carton", definition}};
    DocumentClip card;
    card.id = kClip;
    card.composition_id = kComposition;
    card.source_in = {0, 25};
    card.duration = {kFrames, 25};
    card.timeline_in = {0, 25};
    card.include_audio = false;
    ClipParameterAnimation fade;
    fade.parameter = ClipAnimatedParameter::Opacity;
    fade.keyframes = {Key("01K9R2B0000000000000000031", kFadeFrom, 1),
                      Key("01K9R2B0000000000000000032", kFadeTo, 0)};
    card.animation = {fade};
    document.sequence.tracks = {
        {"01K9R2B0000000000000000002", "video", 0, {card}}};
    return document;
}

bool Export(const Document& document, const std::filesystem::path& directory,
            const std::string& name, std::string& output, std::string& error) {
    output = (directory / (name + ".mkv")).string();
    ExportSettings settings;
    settings.output_path = output;
    settings.encoder = ExportEncoder::LosslessInspection;
    settings.width = kWidth;
    settings.height = kHeight;
    settings.frame_rate = {25, 1};
    settings.ffmpeg_path = FFMPEG_EXECUTABLE;
    ExportPlan plan;
    return Exporter::BuildPlan(document,
                               (directory / (name + ".json")).string(),
                               settings, plan, error) &&
           Exporter::Run(plan, {}, nullptr, error);
}

// The opacity the engine samples at a frame of the card: the expected
// strength of the white over black, before any colour transform.
double Opacity(int64_t frame) {
    if (frame <= kFadeFrom) return 1.0;
    if (frame >= kFadeTo) return 0.0;
    return 1.0 - static_cast<double>(frame - kFadeFrom) /
                     static_cast<double>(kFadeTo - kFadeFrom);
}

void CheckUnmanagedFade(const std::filesystem::path& directory) {
    const Document document = EndCard(false);
    std::string error;
    Check(document.Validate(error),
          "unmanaged: the end card is a valid document: " + error);
    std::string output;
    if (!Export(document, directory, "unmanaged", output, error)) {
        Check(false,
              "unmanaged: a motion clip whose opacity moves exports: " + error);
        return;
    }
    std::vector<double> luma;
    if (!ReadMeanLuma(output, luma) ||
        luma.size() != static_cast<size_t>(kFrames)) {
        Check(false, "unmanaged: the export reads back as " +
                         std::to_string(kFrames) + " frames, got " +
                         std::to_string(luma.size()));
        return;
    }
    // Over black, unmanaged, white at opacity o is o of the full range, to
    // the 8-bit alpha code the graph carries and the encoder's rounding.
    for (int64_t frame = 0; frame < kFrames; ++frame) {
        const double expected = Opacity(frame);
        const double shown = luma[static_cast<size_t>(frame)];
        Check(std::fabs(shown - expected) <= 0.01,
              "unmanaged: frame " + std::to_string(frame) + " shows " +
                  std::to_string(shown) + ", the opacity sampled there is " +
                  std::to_string(expected));
    }

    ExportVerificationSettings verification;
    verification.ffmpeg_path = FFMPEG_EXECUTABLE;
    ExportVerificationReport report;
    // R2 -- a frame of a clip whose opacity moves is never compared with a
    // picture it does not show at full strength: all 40 are unverifiable,
    // and the measure above is what judges them.
    Check(VerifyExport(document, directory, output, verification, report,
                       error) &&
              report.exported_frames == kFrames &&
              report.unverifiable_frames == kFrames &&
              report.divergent_frames == 0,
          "unmanaged: verify_export reads the faded card back, no frame "
          "diverges: " +
              error + " " + SerializeExportVerification(report));
}

void CheckManagedFade(const std::filesystem::path& directory) {
    const Document document = EndCard(true);
    std::string error;
    std::string output;
    if (!Export(document, directory, "managed", output, error)) {
        Check(false,
              "managed: a motion clip whose opacity moves exports under "
              "colour management: " +
                  error);
        return;
    }
    std::vector<double> luma;
    if (!ReadMeanLuma(output, luma) ||
        luma.size() != static_cast<size_t>(kFrames)) {
        Check(false, "managed: the export reads back as " +
                         std::to_string(kFrames) + " frames");
        return;
    }
    // Mixed in the working space, the ramp is not linear in code values:
    // what holds is the shape -- the card at full strength until the fade,
    // fading every frame, and black on the last one.
    const double full = luma[0];
    Check(full > 0.8, "managed: the card is white before the fade, " +
                          std::to_string(full));
    for (int64_t frame = 1; frame <= kFadeFrom; ++frame) {
        Check(std::fabs(luma[static_cast<size_t>(frame)] - full) <= 0.002,
              "managed: frame " + std::to_string(frame) +
                  " holds the card's full strength");
    }
    for (int64_t frame = kFadeFrom + 1; frame <= kFadeTo; ++frame) {
        Check(luma[static_cast<size_t>(frame)] <
                  luma[static_cast<size_t>(frame - 1)],
              "managed: frame " + std::to_string(frame) +
                  " is darker than the one before it");
    }
    Check(luma[static_cast<size_t>(kFadeTo)] <= 0.01,
          "managed: the card's last frame is black, " +
              std::to_string(luma[static_cast<size_t>(kFadeTo)]));
}

// Only the opacity of a motion clip may move: SetClipAnimation refuses an
// animated framing on it, and a document that carries one anyway is refused
// by name rather than delivered with its movement dropped.
void CheckFramingKeysStayRefused(const std::filesystem::path& directory) {
    Document document = EndCard(false);
    ClipParameterAnimation zoom;
    zoom.parameter = ClipAnimatedParameter::FramingScale;
    ClipKeyframe from = Key("01K9R2B0000000000000000041", 0, 1);
    ClipKeyframe to = Key("01K9R2B0000000000000000042", kFadeTo, 2);
    zoom.keyframes = {from, to};
    document.sequence.tracks[0].clips[0].animation.push_back(zoom);
    std::string error;
    std::string output;
    const bool exported = Export(document, directory, "zoomed", output, error);
    Check(!exported && error.find(kClip) != std::string::npos,
          "a motion clip whose framing moves is refused by name: " + error);
}

}  // namespace

int main() {
    const auto directory =
        std::filesystem::temp_directory_path() / ("r2b-" + GenerateUlid());
    std::filesystem::create_directories(directory);
    if (!WriteWhitePng(directory / "white.png")) {
        std::cerr << "FAIL: FFmpeg writes the stand-in card\n";
        return 1;
    }
    SetMotionFrameRenderer(WhiteCard);
    try {
        CheckUnmanagedFade(directory);
        CheckManagedFade(directory);
        CheckFramingKeysStayRefused(directory);
    } catch (const std::exception& exception) {
        ++failures;
        std::cerr << "FAIL: " << exception.what() << '\n';
    }
    SetMotionFrameRenderer(nullptr);
    std::filesystem::remove_all(directory);
    if (failures == 0) {
        std::cout << "PASS: motion clip opacity export\n";
        return 0;
    }
    std::cerr << failures << " failure(s)\n";
    return 1;
}
