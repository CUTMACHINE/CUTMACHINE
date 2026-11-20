// PA15 -- docs/roadmap/PARITE-2026-11.md. The roto (PersonMatte.h):
//
//   - the matte's file: written, read back frame by frame, an index past
//     the end clamped, a file cut short refused, nothing left half-written;
//   - a matte computed through a segmenter that stands for Vision: one
//     matte frame per decoded frame, resampled to the kept size, stopped on
//     demand without a file;
//   - the document: a 'person' mask with a geometry refused, version 27,
//     an older version refused;
//   - the export of a clip cut out by a matte that changes from frame to
//     frame, over a clip below: the boundary where it must be at a frame,
//     at an offset in the source, at a speed, inverted, under colour
//     management, translucent;
//   - read_frame of the same clip;
//   - Vision itself, opened, fed two frames and closed.

#include "Document.h"
#include "Export.h"
#include "FrameCapture.h"
#include "PersonMatte.h"
#include "Ulid.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "TestSupport.h"

namespace {

namespace fs = std::filesystem;

std::string Quote(const std::string& text) { return "'" + text + "'"; }

bool Run(const std::string& command) {
    return std::system(command.c_str()) == 0;
}

std::string ReadAll(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(file)), {});
}

// The matte used below: frame n keeps the columns left of 40 + 10 n.
constexpr int kWidth = 320;
constexpr int kHeight = 180;
int Boundary(int frame) { return 40 + 10 * frame; }

std::vector<uint8_t> MatteFrame(int frame) {
    std::vector<uint8_t> bytes(static_cast<size_t>(kWidth) * kHeight, 0);
    for (int y = 0; y < kHeight; ++y)
        for (int x = 0; x < Boundary(frame) && x < kWidth; ++x)
            bytes[static_cast<size_t>(y) * kWidth + x] = 255;
    return bytes;
}

// A segmenter that stands for Vision: the matte of a picture is a small
// grey picture whose value counts the frames it was handed.
struct FakeSession {
    int frames = 0;
};
void* FakeOpen(const std::string&, std::string&) { return new FakeSession; }
bool FakeSegment(void* opaque, int32_t width, int32_t height,
                 const uint8_t* rgb8, int32_t& matteWidth, int32_t& matteHeight,
                 std::vector<uint8_t>& matte, std::string& error) {
    auto* session = static_cast<FakeSession*>(opaque);
    if (width <= 0 || height <= 0 || rgb8 == nullptr) {
        error = "no picture";
        return false;
    }
    matteWidth = 8;
    matteHeight = 6;
    matte.assign(48, static_cast<uint8_t>(10 * session->frames));
    ++session->frames;
    return true;
}
void FakeClose(void* opaque) { delete static_cast<FakeSession*>(opaque); }
const person_matte::Segmenter kFake{&FakeOpen, &FakeSegment, &FakeClose};

}  // namespace

int main() {
    const fs::path root =
        fs::temp_directory_path() / ("cutmachine-roto-" + GenerateUlid());
    fs::create_directories(root);
    std::string error;

    // ---- sizes and frames ----
    {
        int32_t width = 0;
        int32_t height = 0;
        person_matte::FittedSize(1080, 1920, 1024, width, height);
        Check(width == 576 && height == 1024, "a vertical 1080p is 576x1024");
        person_matte::FittedSize(320, 180, 1024, width, height);
        Check(width == 320 && height == 180, "a small picture keeps its size");
        person_matte::FittedSize(3840, 1606, 1024, width, height);
        Check(width == 1024 && height == 428,
              "both sides even: " + std::to_string(height));
        LibraryMedia turned;
        turned.width = 1920;
        turned.height = 1080;
        turned.rotation_degrees = -90;
        int64_t shownWidth = 0;
        int64_t shownHeight = 0;
        Check(person_matte::DisplayedSize(turned, shownWidth, shownHeight) &&
                  shownWidth == 1080 && shownHeight == 1920,
              "a quarter turn swaps the sides");
        Check(
            person_matte::SourceFrameAt({0, 25}, {25, 1}) == 0 &&
                person_matte::SourceFrameAt({24, 25}, {25, 1}) == 24 &&
                person_matte::SourceFrameAt({1001, 30000}, {30000, 1001}) ==
                    1 &&
                person_matte::SourceFrameAt({1000, 30000}, {30000, 1001}) == 0,
            "the frame a time falls in, floored");
        const std::vector<uint8_t> two = {0, 255};
        std::vector<uint8_t> four(4, 1);
        person_matte::Resample(2, 1, two.data(), 4, 1, four.data());
        Check(four[0] == 0 && four[3] == 255 && four[1] > 0 && four[1] < 128 &&
                  four[2] > 128 && four[2] < 255,
              "bilinear, pixel centres aligned");
    }

    // ---- the file ----
    const fs::path matteFile = root / "film.matte";
    {
        person_matte::Writer writer;
        Check(writer.Open(matteFile.string(), kWidth, kHeight, {25, 1},
                          "accurate", error),
              "the writer opens: " + error);
        for (int frame = 0; frame < 25; ++frame) {
            const std::vector<uint8_t> bytes = MatteFrame(frame);
            Check(writer.Add(bytes.data(), error), "frame written: " + error);
        }
        Check(!fs::exists(matteFile), "nothing at the path before Finish");
        Check(writer.Finish(error), "finished: " + error);
        person_matte::Reader reader;
        Check(reader.Open(matteFile.string(), error) &&
                  reader.info().frames == 25 && reader.info().width == kWidth &&
                  reader.info().height == kHeight &&
                  reader.info().rate.num == 25 &&
                  reader.info().quality == "accurate" &&
                  reader.info().model == person_matte::kModel,
              "read back with its header: " + error);
        std::vector<uint8_t> frame;
        Check(reader.Frame(7, frame, error) && frame == MatteFrame(7),
              "frame 7 is frame 7");
        Check(reader.Frame(400, frame, error) && frame == MatteFrame(24) &&
                  reader.Frame(-3, frame, error) && frame == MatteFrame(0),
              "an index outside reads the nearest frame");
        Check(fs::file_size(matteFile) < size_t{25} * kWidth * kHeight / 10,
              "compressed: " + std::to_string(fs::file_size(matteFile)));

        const fs::path cut = root / "cut.matte";
        const std::string bytes = ReadAll(matteFile);
        std::ofstream(cut, std::ios::binary)
            << bytes.substr(0, bytes.size() - 40);
        person_matte::Info info;
        Check(!person_matte::ReadInfo(cut.string(), info, error) &&
                  error.find("index") != std::string::npos,
              "a file cut short is refused: " + error);

        const fs::path raw = root / "frames.gray";
        Check(person_matte::WriteRawFrames(matteFile.string(), 23, 4,
                                           raw.string(), error) &&
                  fs::file_size(raw) == size_t{4} * kWidth * kHeight,
              "raw frames, the last repeated past the end: " + error);
        const std::string written = ReadAll(raw);
        const std::vector<uint8_t> last = MatteFrame(24);
        Check(written.compare(size_t{3} * kWidth * kHeight, last.size(),
                              std::string(last.begin(), last.end())) == 0,
              "the fourth frame read is the last one");
        const fs::path pgm = root / "frame.pgm";
        Check(person_matte::WriteFramePgm(matteFile.string(), 3, pgm.string(),
                                          error) &&
                  ReadAll(pgm).rfind("P5\n320 180\n255\n", 0) == 0,
              "one frame as a PGM: " + error);
        const std::string filter = person_matte::MatteFilter(
            4, 1080, 1920, "fps=25,scale=540:960", true, "7", 12);
        Check(
            filter.rfind("null[pmp7];[4:v:0]scale=1080:1920", 0) == 0 &&
                filter.find(",fps=25,scale=540:960,trim=end_frame=12,"
                            "negate,format=gray[pmm7]") != std::string::npos &&
                filter.find("[pmp7][pmm7]alphamerge") != std::string::npos,
            "the filter reads the matte through the picture's chain: " +
                filter);
    }

    // ---- computed through a segmenter ----
    const fs::path rush = root / "rush.mp4";
    Check(Run(Quote(FFMPEG_EXECUTABLE) +
              " -hide_banner -loglevel error -f lavfi -i "
              "'testsrc2=size=160x90:rate=25:duration=0.4' -c:v libx264 "
              "-pix_fmt yuv420p -y " +
              Quote(rush.string())),
          "a ten-frame rush");
    {
        Document document;
        LibraryMedia media;
        media.id = "01K4F7W100000000000000000A";
        media.path = rush.string();
        media.filename = "rush.mp4";
        media.has_video = true;
        media.width = 160;
        media.height = 90;
        media.rate = {25, 1};
        media.duration = {10, 25};
        media.metadata_complete = true;
        document.library.push_back(media);
        const fs::path package = root / "computed";
        fs::create_directories(package);
        person_matte::ComputeResult result;
        person_matte::SetSegmenter(nullptr);
        Check(!person_matte::Compute(document, package.string(), media.id,
                                     "accurate", "", {}, result, error) &&
                  error.find("Vision") != std::string::npos,
              "without a segmenter, refused by name: " + error);
        person_matte::SetSegmenter(&kFake);
        int64_t reported = 0;
        Check(person_matte::Compute(
                  document, package.string(), media.id, "accurate", "",
                  [&](int64_t done, int64_t announced) {
                      reported = done;
                      return announced == 10;
                  },
                  result, error),
              "computed: " + error);
        Check(result.info.frames == 10 && result.announced_frames == 10 &&
                  reported == 10 && result.info.width == 160 &&
                  result.info.height == 90,
              "one matte frame per decoded frame, at the picture's size");
        person_matte::Reader reader;
        std::vector<uint8_t> frame;
        Check(reader.Open(result.path, error) &&
                  reader.Frame(6, frame, error) && frame.size() == 160 * 90 &&
                  frame[0] == 60 && frame[160 * 90 - 1] == 60,
              "frame 6 is the segmenter's sixth answer, resampled");
        Check(
            result.path == person_matte::MattePath(package.string(), media.id),
            "kept in the package");
        const fs::path stopped = root / "stopped";
        fs::create_directories(stopped);
        Check(!person_matte::Compute(
                  document, stopped.string(), media.id, "accurate", "",
                  [](int64_t done, int64_t) { return done < 3; }, result,
                  error) &&
                  !fs::exists(
                      person_matte::MattePath(stopped.string(), media.id)) &&
                  !fs::exists(
                      person_matte::MattePath(stopped.string(), media.id) +
                      ".partial"),
              "stopped, nothing kept: " + error);
        Check(!person_matte::Compute(document, package.string(), media.id,
                                     "best", "", {}, result, error),
              "an unknown quality is refused");
        person_matte::SetSegmenter(nullptr);
    }

    // ---- the document ----
    Document layered;
    const fs::path redPath = root / "red.mp4";
    const fs::path picturePath = root / "white.mp4";
    Check(Run(Quote(FFMPEG_EXECUTABLE) +
              " -hide_banner -loglevel error -f lavfi -i "
              "'color=c=red:size=320x180:rate=25:duration=1' -c:v libx264 "
              "-pix_fmt yuv420p -y " +
              Quote(redPath.string())) &&
              Run(Quote(FFMPEG_EXECUTABLE) +
                  " -hide_banner -loglevel error -f lavfi -i "
                  "'color=c=white:size=320x180:rate=25:duration=1' -c:v "
                  "libx264 -pix_fmt yuv420p -y " +
                  Quote(picturePath.string())),
          "the two colour sources");
    layered.sequence.width = kWidth;
    layered.sequence.height = kHeight;
    layered.sequence.frame_rate = {25, 1};
    layered.sources = {
        {"01K4F7W100000000000000000R", redPath.string(), {25, 1}, {25, 25}},
        {"01K4F7W100000000000000000W",
         picturePath.string(),
         {25, 1},
         {25, 25}}};
    for (const DocumentSource& source : layered.sources) {
        LibraryMedia media;
        media.id = source.id;
        media.path = source.path;
        media.filename = fs::path(source.path).filename();
        media.codec = "h264";
        media.orientation = "landscape";
        media.has_video = true;
        media.width = kWidth;
        media.height = kHeight;
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
    above.mask.shape = kClipMaskPerson;
    layered.sequence.tracks = {{GenerateUlid(), "video", 0, {below}},
                               {GenerateUlid(), "video", 1, {above}}};
    {
        Check(layered.Validate(error), "a person mask validates: " + error);
        const std::string saved = layered.SaveToString();
        const size_t version = saved.find("\"version\"");
        Check(version != std::string::npos &&
                  saved.substr(version, 24).find("27") != std::string::npos,
              "a document with a person mask declares version 27");
        Document back;
        Check(Document::LoadFromString(saved, back, error) &&
                  back.sequence.tracks[1].clips[0].mask.shape == "person",
              "read back: " + error);
        std::string older = saved;
        if (version != std::string::npos) {
            const size_t at = older.find("27", version);
            if (at != std::string::npos) older.replace(at, 2, "26");
        }
        Check(!Document::LoadFromString(older, back, error) &&
                  error.find("person") != std::string::npos,
              "version 26 cannot hold it: " + error);
        Document placed = layered;
        placed.sequence.tracks[1].clips[0].mask.width = {1, 2};
        Check(!placed.Validate(error) &&
                  error.find("person") != std::string::npos,
              "a person mask with a size is refused: " + error);
        Document graphic = layered;
        graphic.library[1].color_input.graphics = true;
        Check(!graphic.Validate(error) &&
                  error.find("graphic") != std::string::npos,
              "nor on a graphic: " + error);
    }

    // ---- the export ----
    const fs::path package = root / "Film.cutmachine-project";
    fs::create_directories(package);
    const fs::path project = package / "project.cutmachine.json";
    std::string rgb;
    int exported = 0;
    const auto render = [&](const Document& document, bool managed, int frame) {
        Document subject = document;
        subject.color_management.enabled = managed;
        ExportSettings settings = Exporter::H264Preset(
            (root / ("roto-" + std::to_string(++exported) + ".mp4")).string());
        settings.width = kWidth;
        settings.height = kHeight;
        settings.frame_rate = {25, 1};
        ExportPlan plan;
        if (!Exporter::BuildPlan(subject, project.string(), settings, plan,
                                 error) ||
            !Exporter::Run(plan, {}, nullptr, error))
            return false;
        const fs::path raw = root / "roto.rgb";
        if (!Run(Quote(FFMPEG_EXECUTABLE) + " -v error -xerror -i " +
                 Quote(settings.output_path) + " -map 0:v -vf select='eq(n\\," +
                 std::to_string(frame) +
                 ")' -frames:v 1 -pix_fmt rgb24 -f rawvideo -y " +
                 Quote(raw.string())))
            return false;
        rgb = ReadAll(raw);
        return rgb.size() == size_t{kWidth} * kHeight * 3;
    };
    const auto pixel = [&](int x, int channel) {
        const size_t offset = (static_cast<size_t>(90) * kWidth + x) * 3;
        return static_cast<int>(
            static_cast<unsigned char>(rgb[offset + channel]));
    };
    // White comes out at 210 through the managed view (display transform of
    // a Rec.709 white), red at (208, 1): thresholds that hold both chains.
    const auto white = [&](int x) {
        return pixel(x, 0) > 185 && pixel(x, 1) > 185;
    };
    const auto red = [&](int x) {
        return pixel(x, 0) > 150 && pixel(x, 1) < 60;
    };
    Check(!render(layered, false, 4) &&
              error.find("analyze_person_matte") != std::string::npos,
          "without its matte the export is refused by name: " + error);
    const std::string kept =
        person_matte::MattePath(package.string(), layered.sources[1].id);
    fs::create_directories(fs::path(kept).parent_path());
    fs::copy_file(matteFile, kept, fs::copy_options::overwrite_existing);

    for (const bool managed : {false, true}) {
        const std::string chain = managed ? " (managed)" : " (unmanaged)";
        // Frame 4 of the clip is frame 4 of the media: kept left of 80.
        Check(render(layered, managed, 4) && white(Boundary(4) - 12) &&
                  red(Boundary(4) + 12) && white(10) && red(300),
              "frame 4 keeps the matte of frame 4" + chain + ": " + error);
        Check(render(layered, managed, 9) && white(Boundary(9) - 12) &&
                  red(Boundary(9) + 12),
              "frame 9 keeps the matte of frame 9" + chain + ": " + error);
        // Five frames into the source: frame 4 shows source frame 9.
        Document offset = layered;
        offset.sequence.tracks[1].clips[0].source_in = {5, 25};
        Check(render(offset, managed, 4) && white(Boundary(9) - 12) &&
                  red(Boundary(9) + 12),
              "a clip that starts later reads the matte later" + chain + ": " +
                  error);
        // Twice as fast: frame 4 shows source frame 8.
        Document fast = layered;
        fast.sequence.tracks[1].clips[0].speed = {2, 1};
        Check(render(fast, managed, 4) && white(Boundary(8) - 6) &&
                  red(Boundary(8) + 6),
              "a clip at twice the speed reads its matte at twice the "
              "speed" +
                  chain + ": " + error);
        Document inverted = layered;
        inverted.sequence.tracks[1].clips[0].mask.inverted = true;
        Check(render(inverted, managed, 4) && red(Boundary(4) - 12) &&
                  white(Boundary(4) + 12),
              "inverted, the people are cut out" + chain + ": " + error);
    }
    // A translucent clip keeps its opacity on what the matte keeps.
    {
        Document translucent = layered;
        translucent.sequence.tracks[1].clips[0].opacity = {1, 2};
        Check(render(translucent, false, 4), "rendered: " + error);
        const int g = pixel(20, 1);
        Check(g > 90 && g < 170 && red(300),
              "half opaque where the matte keeps it, the clip below "
              "elsewhere: g=" +
                  std::to_string(g));
    }

    // ---- read_frame ----
    {
        Document document = layered;
        FrameCaptureSettings settings;
        settings.max_dimension = kWidth;
        settings.package_directory = package.string();
        Check(ApplyClipFramingToCapture(document,
                                        document.sequence.tracks[1].clips[0].id,
                                        {4, 25}, settings, error) &&
                  !settings.matte_path.empty() && settings.matte_frame == 4,
              "read_frame finds the matte of frame 4: " + error);
        std::string jpeg;
        Check(CaptureSourceFrame(picturePath.string(), {4, 25}, settings, jpeg,
                                 error),
              "the frame is read: " + error);
        const fs::path picture = root / "read.jpg";
        std::ofstream(picture, std::ios::binary) << jpeg;
        const fs::path raw = root / "read.rgb";
        Check(Run(Quote(FFMPEG_EXECUTABLE) + " -v error -i " +
                  Quote(picture.string()) + " -pix_fmt rgb24 -f rawvideo -y " +
                  Quote(raw.string())),
              "decoded");
        rgb = ReadAll(raw);
        Check(rgb.size() == size_t{kWidth} * kHeight * 3 &&
                  white(Boundary(4) - 12) && pixel(Boundary(4) + 12, 0) < 40,
              "read_frame shows the clip where the matte keeps it, black "
              "elsewhere");
        FrameCaptureSettings lost;
        Check(!ApplyClipFramingToCapture(
                  document, document.sequence.tracks[1].clips[0].id, {4, 25},
                  lost, error),
              "without its package, read_frame says so: " + error);
    }

    // ---- Vision ----
    {
        person_matte::InstallVisionSegmenter();
        const person_matte::Segmenter* vision =
            person_matte::CurrentSegmenter();
        std::string message;
        void* session =
            vision != nullptr
                ? vision->open(person_matte::kQualityBalanced, message)
                : nullptr;
        Check(session != nullptr, "Vision opens a session: " + message);
        if (session != nullptr) {
            std::vector<uint8_t> picture(size_t{64} * 48 * 3, 128);
            std::vector<uint8_t> matte;
            int32_t width = 0;
            int32_t height = 0;
            const bool first = vision->segment(session, 64, 48, picture.data(),
                                               width, height, matte, message);
            const bool second = vision->segment(session, 64, 48, picture.data(),
                                                width, height, matte, message);
            Check(first && second && width > 0 && height > 0 &&
                      matte.size() == static_cast<size_t>(width) * height,
                  "Vision answers a matte for each frame: " + message);
            vision->close(session);
        }
        person_matte::SetSegmenter(nullptr);
    }

    std::error_code ignored;
    fs::remove_all(root, ignored);
    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "person matte tests passed\n";
    return 0;
}
