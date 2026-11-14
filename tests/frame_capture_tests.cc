#include "FrameCapture.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

int failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

std::string Quote(const std::filesystem::path& path) {
    std::string result = "'";
    for (char character : path.string())
        result += character == '\'' ? "'\\''" : std::string(1, character);
    return result + "'";
}

}  // namespace

int main() {
    // ---- base64, against the RFC 4648 test vectors -----------------------
    Check(EncodeBase64("") == "", "the empty string encodes to nothing");
    Check(EncodeBase64("f") == "Zg==", "one byte pads with two '='");
    Check(EncodeBase64("fo") == "Zm8=", "two bytes pad with one '='");
    Check(EncodeBase64("foo") == "Zm9v", "three bytes need no padding");
    Check(EncodeBase64("foob") == "Zm9vYg==", "four bytes");
    Check(EncodeBase64("fooba") == "Zm9vYmE=", "five bytes");
    Check(EncodeBase64("foobar") == "Zm9vYmFy", "six bytes");
    // Bytes above 0x7f must not sign-extend: a JPEG is full of them, and
    // this is exactly where a char-vs-unsigned-char slip corrupts an image.
    const std::string high("\xff\xfe\xfd", 3);
    Check(EncodeBase64(high) == "//79",
          "high bytes encode unsigned, so JPEG data survives");

    // ---- a real frame, decoded by FFmpeg ---------------------------------
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "cutmachine-frame-tests";
    std::filesystem::create_directories(root);
    const std::filesystem::path source = root / "vertical.mp4";
    const std::string generate =
        Quote(FFMPEG_EXECUTABLE) +
        " -hide_banner -loglevel error -f lavfi -i "
        "'testsrc2=size=180x320:rate=25:duration=2' -c:v libx264 "
        "-pix_fmt yuv420p -y " +
        Quote(source);
    if (std::system(generate.c_str()) != 0) {
        std::cerr << "FAIL: unable to generate the frame fixture\n";
        std::filesystem::remove_all(root);
        return 1;
    }

    FrameCaptureSettings settings;
    settings.ffmpeg_path = FFMPEG_EXECUTABLE;
    std::string jpeg;
    std::string error;
    Check(CaptureSourceFrame(source.string(), {25, 25}, settings, jpeg, error),
          "a frame renders: " + error);
    // JPEG's own magic. Checked rather than assumed: the whole point of this
    // path is that a model receives a picture, and a truncated or misnamed
    // payload would be silently useless.
    Check(jpeg.size() > 512, "the frame has a plausible size");
    Check(jpeg.size() >= 3 && static_cast<unsigned char>(jpeg[0]) == 0xFF &&
              static_cast<unsigned char>(jpeg[1]) == 0xD8 &&
              static_cast<unsigned char>(jpeg[2]) == 0xFF,
          "the bytes really are a JPEG");

    // Two renders of the same position must be identical, so a frame handed
    // to a model is reproducible like every other measurement here.
    std::string again;
    Check(
        CaptureSourceFrame(source.string(), {25, 25}, settings, again, error) &&
            again == jpeg,
        "rendering the same position twice gives identical bytes");

    // A different position must give a different picture -- this is what
    // catches a seek that silently ignores its argument.
    std::string other;
    Check(
        CaptureSourceFrame(source.string(), {5, 25}, settings, other, error) &&
            other != jpeg,
        "a different position gives a different frame");

    // Exact seconds, not a float: 1/25 must become "0.04", never "0.039999".
    std::string early;
    Check(CaptureSourceFrame(source.string(), {1, 25}, settings, early, error),
          "an awkward rational position renders: " + error);

    // ITW5 -- with colour management, read_frame shows the display view the
    // timeline sheets show, not the raw signal.
    FrameCaptureSettings managed = settings;
    managed.color_management.enabled = true;
    managed.color_management.input_gamut = "sony_sgamut3_cine";
    managed.color_management.input_transfer = "sony_slog3";
    std::string transformed;
    Check(CaptureSourceFrame(source.string(), {25, 25}, managed, transformed,
                             error) &&
              transformed.size() > 512 && transformed != jpeg,
          "a managed frame goes through the display transform: " + error);

    // R1 -- docs/roadmap/REEL-2026-09.md. read_frame of a framed clip shows
    // what the sequence shows, not the whole rush: the agent that reframed
    // a shot has to look at the composed picture (IA4). Here through the
    // same path the two backends take, ApplyClipFramingToCapture.
    {
        Document document;
        document.sequence.width = 320;
        document.sequence.height = 180;
        document.sequence.frame_rate = {25, 1};
        LibraryMedia media;
        media.id = "01K80000000000000000000001";
        media.path = source.string();
        media.filename = "vertical.mp4";
        media.codec = "h264";
        media.orientation = "portrait";
        media.has_video = true;
        media.width = 180;
        media.height = 320;
        media.rate = {25, 1};
        media.duration = {50, 25};
        document.library.push_back(media);
        document.sources.push_back(
            {media.id, media.path, media.rate, media.duration});
        DocumentClip clip;
        clip.id = "01K80000000000000000000011";
        clip.source_id = media.id;
        clip.source_in = {0, 25};
        clip.duration = {25, 25};
        clip.timeline_in = {0, 25};
        clip.include_audio = false;
        clip.framing.mode = FramingMode::Fill;
        document.sequence.tracks = {
            {"01K80000000000000000000021", "video", 0, {clip}}};
        std::string validationError;
        Check(document.Validate(validationError),
              "the framed fixture validates: " + validationError);

        FrameCaptureSettings framed = settings;
        Check(ApplyClipFramingToCapture(document, clip.id, {0, 25}, framed,
                                        error) &&
                  framed.framed && framed.frame_width > framed.frame_height,
              "the capture is set up in the sequence's own shape: " + error);
        std::string composed;
        Check(CaptureSourceFrame(source.string(), {0, 25}, framed, composed,
                                 error) &&
                  composed.size() > 512 && composed != jpeg,
              "a framed clip's frame is composed, not the rush: " + error);

        FrameCaptureSettings plain = settings;
        Check(ApplyClipFramingToCapture(document, Ulid(), {0, 25}, plain,
                                        error) &&
                  !plain.framed,
              "a media read with no clip is left alone: " + error);
        Document unframed = document;
        unframed.sequence.tracks[0].clips[0].framing = ClipFraming{};
        FrameCaptureSettings untouched = settings;
        Check(ApplyClipFramingToCapture(unframed, clip.id, {0, 25}, untouched,
                                        error) &&
                  !untouched.framed,
              "and so is a clip that frames nothing: " + error);

        // EF3 -- docs/roadmap/EFFETS-2026-10.md. A blurred clip is composed
        // into the sequence's frame as a framed one is, its blur carried
        // into that frame's pixels and drawn before the frame is laid
        // around the picture.
        Document blurred = unframed;
        blurred.sequence.tracks[0].clips[0].blur = {10, 1};
        FrameCaptureSettings soft = settings;
        Check(
            ApplyClipFramingToCapture(blurred, clip.id, {0, 25}, soft, error) &&
                soft.framed &&
                soft.blur_filter.rfind("gblur=sigma=(", 0) == 0 &&
                !soft.blur_rgb,
            "a blurred clip is composed, with its blur: " + error +
                soft.blur_filter);
        std::string softened;
        std::string sharp;
        FrameCaptureSettings sharpSettings = soft;
        sharpSettings.blur_filter.clear();
        Check(CaptureSourceFrame(source.string(), {0, 25}, soft, softened,
                                 error) &&
                  CaptureSourceFrame(source.string(), {0, 25}, sharpSettings,
                                     sharp, error) &&
                  softened.size() > 256 && softened != sharp,
              "FFmpeg draws the blur, and the frame is not the sharp one: " +
                  error);

        // PA8 -- docs/roadmap/PARITE-2026-11.md. A masked clip is composed as a
        // framed one, its mask put on the picture; what it cuts out is black.
        {
            Document masked = document;
            DocumentClip& first = masked.sequence.tracks[0].clips[0];
            first.framing = ClipFraming{};
            first.mask.shape = kClipMaskEllipse;
            first.mask.width = {1, 2};
            first.mask.feather = {1, 4};
            FrameCaptureSettings cut = settings;
            Check(
                ApplyClipFramingToCapture(masked, first.id, {0, 25}, cut,
                                          error) &&
                    cut.framed &&
                    cut.mask_filter.find("alphaextract") != std::string::npos &&
                    cut.mask_filter.find("hypot(") != std::string::npos,
                "a masked clip is composed, with its mask: " + error +
                    cut.mask_filter);
            std::string kept;
            std::string whole;
            FrameCaptureSettings uncut = cut;
            uncut.mask_filter.clear();
            Check(
                CaptureSourceFrame(source.string(), {0, 25}, cut, kept,
                                   error) &&
                    CaptureSourceFrame(source.string(), {0, 25}, uncut, whole,
                                       error) &&
                    kept.size() > 256 && kept != whole,
                "FFmpeg draws the mask, and the frame is not the whole one: " +
                    error);
        }
    }

    Check(!CaptureSourceFrame(source.string(), {-1, 25}, settings, jpeg, error),
          "a negative position is refused");
    Check(!CaptureSourceFrame((root / "missing.mp4").string(), {0, 25},
                              settings, jpeg, error),
          "a missing source is refused rather than returning empty bytes");

    std::filesystem::remove_all(root);
    if (failures == 0) std::cout << "frame capture tests passed\n";
    return failures == 0 ? 0 : 1;
}
