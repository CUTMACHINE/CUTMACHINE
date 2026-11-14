// QC-2026-09 (A6) -- selection stays pure and exact; a small generated media
// fixture proves the delivered JPEG crosses the document colour pipeline.

#include "TimelineSheets.h"
#include "Ulid.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
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

Document Fixture() {
    Document document;
    document.sequence.frame_rate = {25, 1};
    document.sources = {
        {"01K30000000000000000000001", "a.mp4", {25, 1}, {1000, 25}},
        {"01K30000000000000000000002", "b.mp4", {25, 1}, {1000, 25}},
        {"01K30000000000000000000003", "c.mp4", {25, 1}, {1000, 25}},
        {"01K30000000000000000000004", "x.mp4", {25, 1}, {1000, 25}},
    };
    document.sequence.tracks = {
        {"01K30000000000000000000010",
         "video",
         0,
         {{"01K30000000000000000000011",
           "01K30000000000000000000001",
           {100, 25},
           {10, 25},
           {0, 25}},
          {"01K30000000000000000000012",
           "01K30000000000000000000002",
           {200, 25},
           {10, 25},
           {10, 25}},
          {"01K30000000000000000000013",
           "01K30000000000000000000003",
           {400, 25},
           {10, 25},
           {20, 25}}}},
        {"01K30000000000000000000020",
         "video",
         1,
         {{"01K30000000000000000000021",
           "01K30000000000000000000004",
           {300, 25},
           {10, 25},
           {10, 25}}}},
    };
    return document;
}

}  // namespace

int main() {
    TimelineSheetSettings settings;
    TimelineSheetPlan plan;
    std::string error;
    const Document document = Fixture();

    Check(BuildTimelineSheetPlan(document, TimelineSheetKind::Contact, settings,
                                 plan, error),
          "contact plan builds: " + error);
    Check(plan.total_candidates == 3 && plan.frames.size() == 3,
          "contact sheet contains only the three composited shots");
    Check(plan.frames.size() == 3 &&
              plan.frames[0].timeline_position == RationalTime{5, 25} &&
              plan.frames[0].source_position == RationalTime{105, 25} &&
              plan.frames[1].clip_id == "01K30000000000000000000021" &&
              plan.frames[1].source_position == RationalTime{305, 25} &&
              plan.frames[2].source_position == RationalTime{405, 25},
          "contact cells are exact middle frames of visible segments");

    settings.maximum_images = 2;
    Check(BuildTimelineSheetPlan(document, TimelineSheetKind::Contact, settings,
                                 plan, error) &&
              plan.frames.size() == 2 &&
              plan.frames.front().clip_id == "01K30000000000000000000011" &&
              plan.frames.back().clip_id == "01K30000000000000000000013",
          "bounded contact sheets sample the whole timeline");

    settings.maximum_images = 24;
    Check(BuildTimelineSheetPlan(document, TimelineSheetKind::Cuts, settings,
                                 plan, error),
          "cut plan builds: " + error);
    Check(plan.total_candidates == 2 && plan.frames.size() == 4,
          "cut sheet keeps two complete visible-cut pairs");
    Check(plan.frames.size() == 4 && plan.frames[0].role == "before" &&
              plan.frames[0].timeline_position == RationalTime{9, 25} &&
              plan.frames[0].source_position == RationalTime{109, 25} &&
              plan.frames[1].role == "after" &&
              plan.frames[1].timeline_position == RationalTime{10, 25} &&
              plan.frames[1].source_position == RationalTime{300, 25} &&
              plan.frames[2].source_position == RationalTime{309, 25} &&
              plan.frames[3].source_position == RationalTime{400, 25},
          "each cut uses the exact sequence frames bracketing it");
    Check(
        SerializeTimelineSheetPlan(plan).find(
            "\"cut_position\":{\"value\":10,\"rate\":25}") != std::string::npos,
        "sheet metadata keeps exact cut positions");
    settings.maximum_images = 1;
    Check(!BuildTimelineSheetPlan(document, TimelineSheetKind::Cuts, settings,
                                  plan, error),
          "a cut sheet never exceeds its bound with an incomplete pair");
    settings.maximum_images = 24;

    // PRD39 -- ROADMAP.md. A plan with nothing to show says why, for every
    // surface to refuse it before the render; a plan with cells says
    // nothing. One default for every sheet, the one the surfaces read.
    {
        Check(TimelineSheetSettings{}.maximum_images == 24,
              "every sheet takes 24 cells by default");
        TimelineSheetPlan full;
        Check(BuildTimelineSheetPlan(document, TimelineSheetKind::Cuts,
                                     settings, full, error) &&
                  EmptyTimelineSheetReason(full).empty(),
              "a plan with cells has no reason to be refused");
        Document apart = document;
        apart.sequence.tracks.pop_back();
        apart.sequence.tracks[0].clips[1].timeline_in = {12, 25};
        apart.sequence.tracks[0].clips[2].timeline_in = {24, 25};
        TimelineSheetPlan uncut;
        Check(
            BuildTimelineSheetPlan(apart, TimelineSheetKind::Cuts, settings,
                                   uncut, error) &&
                uncut.frames.empty() &&
                EmptyTimelineSheetReason(uncut).rfind(
                    "nothing to show: the timeline has no visible cut", 0) == 0,
            "shots apart from each other make no cut, and the plan says "
            "so: " +
                EmptyTimelineSheetReason(uncut));
        Document blank = document;
        for (DocumentTrack& track : blank.sequence.tracks) track.clips.clear();
        TimelineSheetPlan nothing;
        Check(BuildTimelineSheetPlan(blank, TimelineSheetKind::Contact,
                                     settings, nothing, error) &&
                  EmptyTimelineSheetReason(nothing).rfind(
                      "nothing to show: the timeline has no visible video "
                      "shot",
                      0) == 0,
              "an empty timeline has no shot, and the plan says so");
        std::string ignored;
        Check(!RenderTimelineSheet(blank, std::filesystem::path(), nothing,
                                   settings, ignored, error) &&
                  error == EmptyTimelineSheetReason(nothing),
              "the render refuses it in the same words: " + error);
    }

    // PRD39 -- ROADMAP.md. A folder of this run's own: under a fixed name,
    // two suites run at once on one machine (two worktrees) wrote over each
    // other's fixtures, and the first to end removed the other's folder.
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("cutmachine-sheet-tests-" + GenerateUlid());
    std::filesystem::create_directories(root);
    const std::filesystem::path source = root / "grey.mp4";
    const std::string generate =
        Quote(FFMPEG_EXECUTABLE) +
        " -hide_banner -loglevel error -f lavfi -i "
        "'testsrc2=size=160x90:rate=25:duration=1' -c:v libx264 "
        "-pix_fmt yuv420p -y " +
        Quote(source);
    Check(std::system(generate.c_str()) == 0, "colour fixture is generated");

    Document renderDocument;
    LibraryMedia media;
    media.id = "01K31000000000000000000001";
    media.path = source.filename().string();
    media.filename = source.filename().string();
    media.rate = {25, 1};
    media.duration = {25, 25};
    renderDocument.library.push_back(media);
    renderDocument.sources.push_back(
        {media.id, media.path, media.rate, media.duration});
    renderDocument.sequence.tracks = {
        {"01K31000000000000000000002",
         "video",
         0,
         {{"01K31000000000000000000003",
           media.id,
           {0, 25},
           {25, 25},
           {0, 25}}}},
    };
    settings.ffmpeg_path = FFMPEG_EXECUTABLE;
    settings.cell_dimension = 128;
    Check(BuildTimelineSheetPlan(renderDocument, TimelineSheetKind::Contact,
                                 settings, plan, error),
          "render plan builds: " + error);
    std::string unmanaged;
    Check(RenderTimelineSheet(renderDocument, root, plan, settings, unmanaged,
                              error),
          "unmanaged sheet renders: " + error);
    // EF3 -- docs/roadmap/EFFETS-2026-10.md. A blurred clip's cell is the
    // clip blurred, drawn in the cell's pixels, plain and graded.
    {
        Document blurred = renderDocument;
        blurred.library[0].width = 160;
        blurred.library[0].height = 90;
        blurred.library[0].has_video = true;
        blurred.library[0].pixel_format = "yuv420p";
        Document sharp = blurred;
        blurred.sequence.tracks[0].clips[0].blur = {40, 1};
        std::string sharpSheet;
        std::string blurredSheet;
        Check(RenderTimelineSheet(sharp, root, plan, settings, sharpSheet,
                                  error) &&
                  RenderTimelineSheet(blurred, root, plan, settings,
                                      blurredSheet, error) &&
                  blurredSheet.size() > 256 && blurredSheet != sharpSheet &&
                  blurredSheet.size() < sharpSheet.size(),
              "a blurred clip's cell is blurred, a smoother picture: " + error);
        TimelineSheetSettings gradedBlur = settings;
        gradedBlur.graded = true;
        std::string gradedSharp;
        std::string gradedBlurred;
        Check(RenderTimelineSheet(sharp, root, plan, gradedBlur, gradedSharp,
                                  error) &&
                  RenderTimelineSheet(blurred, root, plan, gradedBlur,
                                      gradedBlurred, error) &&
                  gradedBlurred != gradedSharp &&
                  gradedBlurred.size() < gradedSharp.size(),
              "and so is its graded cell: " + error);
    }
    renderDocument.color_management.enabled = true;
    renderDocument.color_management.input_gamut = "sony_sgamut3_cine";
    renderDocument.color_management.input_transfer = "sony_slog3";
    renderDocument.color_management.input_ycbcr_matrix = "bt709";
    renderDocument.color_management.input_range = "full";
    std::string managed;
    Check(RenderTimelineSheet(renderDocument, root, plan, settings, managed,
                              error),
          "colour-managed sheet renders: " + error);
    Check(managed.size() > 512 && managed != unmanaged,
          "document colour management changes the delivered JPEG bytes");
    Check(managed.size() >= 3 &&
              static_cast<unsigned char>(managed[0]) == 0xff &&
              static_cast<unsigned char>(managed[1]) == 0xd8 &&
              static_cast<unsigned char>(managed[2]) == 0xff,
          "the managed sheet is a JPEG image");

    // G3 -- docs/roadmap/GRADE-2026-09.md. Two media from one file, one of
    // them declared Rec.709, under the S-Log3 project above: each cell is
    // read with its own media's input, as it would be in a sheet where both
    // media shared that input.
    {
        Document mixed = renderDocument;
        LibraryMedia phone = mixed.library[0];
        phone.id = "01K31000000000000000000011";
        phone.color_input = {"rec709", "rec709", "auto", "auto"};
        mixed.library.push_back(phone);
        mixed.sources.push_back(
            {phone.id, phone.path, phone.rate, phone.duration});
        mixed.sequence.tracks[0].clips.push_back({"01K31000000000000000000013",
                                                  phone.id,
                                                  {0, 25},
                                                  {25, 25},
                                                  {25, 25}});
        Document allLog = mixed;
        allLog.library[1].color_input = {};
        Document allRec = mixed;
        allRec.library[0].color_input = phone.color_input;
        TimelineSheetPlan mixedPlan;
        std::string rgb[3];
        int64_t columns = 0;
        size_t logCell = 0;
        size_t recCell = 0;
        const Document* sheets[3] = {&mixed, &allLog, &allRec};
        for (int index = 0; index < 3; ++index) {
            std::string jpeg;
            Check(BuildTimelineSheetPlan(*sheets[index],
                                         TimelineSheetKind::Contact, settings,
                                         mixedPlan, error) &&
                      RenderTimelineSheet(*sheets[index], root, mixedPlan,
                                          settings, jpeg, error),
                  "G3: sheet " + std::to_string(index) + " renders: " + error);
            const auto image = root / ("g3-" + std::to_string(index) + ".jpg");
            { std::ofstream(image, std::ios::binary) << jpeg; }
            const auto raw = root / ("g3-" + std::to_string(index) + ".rgb");
            const std::string decode =
                Quote(FFMPEG_EXECUTABLE) +
                " -hide_banner -loglevel error -y -i " + Quote(image) +
                " -f rawvideo -pix_fmt rgb24 " + Quote(raw);
            Check(std::system(decode.c_str()) == 0, "G3: the sheet decodes");
            std::ifstream in(raw, std::ios::binary);
            rgb[index].assign(std::istreambuf_iterator<char>(in),
                              std::istreambuf_iterator<char>());
        }
        columns = std::min<int64_t>(
            settings.columns, static_cast<int64_t>(mixedPlan.frames.size()));
        for (size_t index = 0; index < mixedPlan.frames.size(); ++index) {
            if (mixedPlan.frames[index].source_id == phone.id) {
                recCell = index;
                break;
            }
        }
        Check(recCell > 0 && columns > 0,
              "G3: the sheet holds a frame of each media");
        // The mean of a 16-pixel square at a cell's centre.
        const auto cellMean = [&](const std::string& pixels, size_t cell) {
            const int64_t width = columns * settings.cell_dimension;
            const int64_t cx = (static_cast<int64_t>(cell) % columns) *
                                   settings.cell_dimension +
                               settings.cell_dimension / 2;
            const int64_t cy = (static_cast<int64_t>(cell) / columns) *
                                   settings.cell_dimension +
                               settings.cell_dimension / 2;
            std::array<double, 3> sum{};
            for (int64_t y = cy - 8; y < cy + 8; ++y)
                for (int64_t x = cx - 8; x < cx + 8; ++x)
                    for (int channel = 0; channel < 3; ++channel) {
                        const size_t at = static_cast<size_t>(
                            ((y * width) + x) * 3 + channel);
                        if (at < pixels.size())
                            sum[channel] +=
                                static_cast<unsigned char>(pixels[at]);
                    }
            for (double& value : sum) value /= 256.0;
            return sum;
        };
        const auto gap = [&](const std::array<double, 3>& left,
                             const std::array<double, 3>& right) {
            double largest = 0.0;
            for (int channel = 0; channel < 3; ++channel)
                largest =
                    std::max(largest, std::abs(left[channel] - right[channel]));
            return largest;
        };
        const double logGap =
            gap(cellMean(rgb[0], logCell), cellMean(rgb[1], logCell));
        const double recGap =
            gap(cellMean(rgb[0], recCell), cellMean(rgb[2], recCell));
        const double ignoredGap =
            gap(cellMean(rgb[0], recCell), cellMean(rgb[1], recCell));
        Check(logGap <= 3.0 && recGap <= 3.0 && ignoredGap > 10.0,
              "G3: each cell reads as its own media's input: S-Log3 cell " +
                  std::to_string(logGap) + ", Rec.709 cell " +
                  std::to_string(recGap) + ", against S-Log3 " +
                  std::to_string(ignoredGap));
    }

    // Agentic QA of 23 September 2026 -- at 1/2, the middle of a 23-frame
    // shot is source frame 5.5 by the rule's time, and the monitor shows 5.
    // The sheet seeked to 5.5 and drew 6; it must draw, byte for byte, what
    // a shot at speed 1 whose middle is frame 5 draws.
    renderDocument.color_management.enabled = false;
    const auto sheetOf = [&](int64_t frames, EffectParamValue speed,
                             std::string& jpeg) {
        Document single = renderDocument;
        DocumentClip& clip = single.sequence.tracks[0].clips[0];
        clip.duration = {frames, 25};
        clip.speed = speed;
        TimelineSheetPlan singlePlan;
        if (!BuildTimelineSheetPlan(single, TimelineSheetKind::Contact,
                                    settings, singlePlan, error) ||
            !RenderTimelineSheet(single, root, singlePlan, settings, jpeg,
                                 error))
            return RationalTime{-1, 1};
        return singlePlan.frames[0].source_position;
    };
    std::string slowed;
    std::string fifth;
    std::string sixth;
    const RationalTime slowedAt = sheetOf(23, {1, 2}, slowed);
    Check(slowedAt == RationalTime{5, 25},
          "the slowed shot's cell names frame 5 from its first instant: " +
              error);
    Check(sheetOf(11, {1, 1}, fifth) == RationalTime{5, 25} &&
              sheetOf(13, {1, 1}, sixth) == RationalTime{6, 25},
          "the two unit-speed references read frames 5 and 6: " + error);
    Check(!slowed.empty() && slowed == fifth,
          "at 1/2 the sheet draws source frame 5, as the monitor does");
    Check(fifth != sixth, "frames 5 and 6 give two different sheets");

    // Agentic QA of 23 September 2026 -- a JPEG is BT.601 by definition.
    // A BT.709 red of (192, 32, 32) written into it as it was read back as
    // (177, 16, 32); converted on the way in, it reads back as itself.
    const std::filesystem::path red = root / "red.mp4";
    const std::string generateRed =
        Quote(FFMPEG_EXECUTABLE) +
        " -hide_banner -loglevel error -f lavfi -i "
        "'color=c=0xC02020:s=160x90:r=25:d=1,format=rgb24' "
        "-vf 'scale=out_color_matrix=bt709:out_range=tv,format=yuv420p' "
        "-c:v libx264 -qp 0 -y " +
        Quote(red);
    Check(std::system(generateRed.c_str()) == 0, "red fixture is generated");
    Document redDocument = renderDocument;
    redDocument.library[0].path = red.filename().string();
    redDocument.library[0].filename = red.filename().string();
    redDocument.sources[0].path = red.filename().string();
    std::string redSheet;
    Check(BuildTimelineSheetPlan(redDocument, TimelineSheetKind::Contact,
                                 settings, plan, error) &&
              RenderTimelineSheet(redDocument, root, plan, settings, redSheet,
                                  error),
          "the red sheet renders: " + error);
    { std::ofstream(root / "red.jpg", std::ios::binary) << redSheet; }
    const std::string decode =
        Quote(FFMPEG_EXECUTABLE) + " -hide_banner -loglevel error -y -i " +
        Quote(root / "red.jpg") + " -f rawvideo -pix_fmt rgb24 " +
        Quote(root / "red.rgb");
    Check(std::system(decode.c_str()) == 0, "the red sheet decodes");
    std::ifstream pixels(root / "red.rgb", std::ios::binary);
    std::string decoded((std::istreambuf_iterator<char>(pixels)),
                        std::istreambuf_iterator<char>());
    // The middle of the cell, 128 pixels wide and as tall as the
    // sequence's shape makes it.
    const size_t rows = decoded.size() / (128 * 3);
    const size_t middle = (((rows / 2) * 128) + 64) * 3;
    if (rows > 0 && decoded.size() >= middle + 3) {
        const int r = static_cast<unsigned char>(decoded[middle]);
        const int g = static_cast<unsigned char>(decoded[middle + 1]);
        const int b = static_cast<unsigned char>(decoded[middle + 2]);
        Check(std::abs(r - 192) <= 4 && std::abs(g - 32) <= 4 &&
                  std::abs(b - 32) <= 4,
              "a BT.709 red reads back as itself from the sheet's JPEG, got " +
                  std::to_string(r) + "," + std::to_string(g) + "," +
                  std::to_string(b));
    } else {
        Check(false, "the red sheet decodes to 128-pixel-wide rows");
    }

    // A cell has the sequence's shape. It used to be a square with the
    // frame letterboxed inside: 44 % of every cell of a 16:9 or a 9:16
    // sheet was black, and a rush of another shape than the sequence was
    // fitted to the square, not to the frame the monitor fits it to.
    const auto sheetSize = [&](int32_t width, int32_t height,
                               const std::string& name, int& sheetWidth,
                               int& sheetHeight, int& topRed) {
        Document shaped = redDocument;
        shaped.sequence.width = width;
        shaped.sequence.height = height;
        std::string jpeg;
        TimelineSheetPlan shapedPlan;
        if (!BuildTimelineSheetPlan(shaped, TimelineSheetKind::Contact,
                                    settings, shapedPlan, error) ||
            !RenderTimelineSheet(shaped, root, shapedPlan, settings, jpeg,
                                 error))
            return false;
        { std::ofstream(root / (name + ".jpg"), std::ios::binary) << jpeg; }
        const std::string toPpm = Quote(FFMPEG_EXECUTABLE) +
                                  " -hide_banner -loglevel error -y -i " +
                                  Quote(root / (name + ".jpg")) + " -c:v ppm " +
                                  Quote(root / (name + ".ppm"));
        if (std::system(toPpm.c_str()) != 0) return false;
        std::ifstream ppm(root / (name + ".ppm"), std::ios::binary);
        std::string magic;
        int maximum = 0;
        ppm >> magic >> sheetWidth >> sheetHeight >> maximum;
        ppm.get();
        std::string raster((std::istreambuf_iterator<char>(ppm)),
                           std::istreambuf_iterator<char>());
        // Second row, middle column: black letterbox in a square cell.
        const size_t at = ((static_cast<size_t>(sheetWidth) * 1) +
                           static_cast<size_t>(sheetWidth) / 2) *
                          3;
        topRed = raster.size() >= at + 3
                     ? static_cast<unsigned char>(raster[at])
                     : 0;
        return magic == "P6";
    };
    int landscapeWidth = 0;
    int landscapeHeight = 0;
    int landscapeTop = 0;
    int portraitWidth = 0;
    int portraitHeight = 0;
    int portraitTop = 0;
    Check(sheetSize(1920, 1080, "landscape", landscapeWidth, landscapeHeight,
                    landscapeTop) &&
              landscapeWidth == 128 && landscapeHeight == 72,
          "a 16:9 timeline's cell is 128 x 72, got " +
              std::to_string(landscapeWidth) + " x " +
              std::to_string(landscapeHeight) + ": " + error);
    Check(landscapeTop > 150,
          "the 16:9 picture fills its cell to the top edge, red " +
              std::to_string(landscapeTop));
    Check(sheetSize(1080, 1920, "portrait", portraitWidth, portraitHeight,
                    portraitTop) &&
              portraitWidth == 72 && portraitHeight == 128,
          "a 9:16 timeline's cell is 72 x 128, got " +
              std::to_string(portraitWidth) + " x " +
              std::to_string(portraitHeight) + ": " + error);

    // The derush sheet: rushes, not an edit, and nothing on a timeline.
    {
        Document rushes = redDocument;
        rushes.sequence.tracks.clear();
        rushes.library[0].width = 160;
        rushes.library[0].height = 90;
        const Ulid redId = rushes.library[0].id;
        TimelineSheetSettings mediaSettings = settings;
        mediaSettings.maximum_images = 5;
        TimelineSheetPlan across;
        Check(BuildMediaSheetPlan(rushes, {redId}, mediaSettings, across,
                                  error) &&
                  across.kind == TimelineSheetKind::Media &&
                  across.frames.size() == 5,
              "one rush gives the frames asked for: " + error);
        bool spread = across.frames.size() == 5;
        const int64_t expected[] = {2, 7, 12, 17, 22};
        for (size_t index = 0; spread && index < 5; ++index)
            spread = across.frames[index].source_id == redId &&
                     across.frames[index].clip_id.empty() &&
                     across.frames[index].source_position ==
                         RationalTime{expected[index], 25};
        Check(spread, "each is the middle of one fifth of the 25-frame take");
        Check(mediaSettings.frame_width == 160 &&
                  mediaSettings.frame_height == 90,
              "the cells take the rush's own shape");
        Check(SerializeTimelineSheetPlan(across).find("\"media_sheet\"") !=
                  std::string::npos,
              "the sheet says it is a sheet of rushes");

        Document two = rushes;
        LibraryMedia second = two.library[0];
        second.id = "01K31000000000000000000009";
        second.duration = {50, 25};
        second.rotation_degrees = 90;
        two.library.insert(two.library.begin(), second);
        two.sources.push_back(
            {second.id, second.path, second.rate, second.duration});
        TimelineSheetSettings twoSettings = settings;
        TimelineSheetPlan middles;
        Check(
            BuildMediaSheetPlan(two, {second.id, redId, second.id}, twoSettings,
                                middles, error) &&
                middles.frames.size() == 2 &&
                middles.frames[0].source_position == RationalTime{25, 25} &&
                middles.frames[1].source_position == RationalTime{12, 25},
            "several rushes give each one's middle frame, once each: " + error);
        Check(twoSettings.frame_width == 90 && twoSettings.frame_height == 160,
              "a rush turned a quarter shows upright: its cells too");
        TimelineSheetPlan refused;
        Check(!BuildMediaSheetPlan(rushes, {"01K310000000000000000000ZZ"},
                                   twoSettings, refused, error) &&
                  error.find("unknown media_id") != std::string::npos,
              "an unknown rush is refused by name: " + error);

        std::string jpeg;
        TimelineSheetSettings renderSettings = settings;
        renderSettings.maximum_images = 4;
        TimelineSheetPlan four;
        Check(
            BuildMediaSheetPlan(rushes, {redId}, renderSettings, four, error) &&
                RenderTimelineSheet(rushes, root, four, renderSettings, jpeg,
                                    error),
            "a sheet of one rush renders without a timeline: " + error);
        { std::ofstream(root / "rush.jpg", std::ios::binary) << jpeg; }
        const std::string toPpm =
            Quote(FFMPEG_EXECUTABLE) + " -hide_banner -loglevel error -y -i " +
            Quote(root / "rush.jpg") + " -c:v ppm " + Quote(root / "rush.ppm");
        int width = 0;
        int height = 0;
        if (std::system(toPpm.c_str()) == 0) {
            std::ifstream ppm(root / "rush.ppm", std::ios::binary);
            std::string magic;
            ppm >> magic >> width >> height;
        }
        Check(width == 512 && height == 72,
              "four 16:9 cells side by side, 512 x 72, got " +
                  std::to_string(width) + " x " + std::to_string(height));
    }

    // DEC29 -- docs/roadmap/GRADE-2026-09.md. contact_sheet and cut_sheet
    // with `graded`: each cell is its clip as read_frame graded shows it.
    // Two shots of one flat grey, the second graded two stops up, side by
    // side. Code 104 is 0.4064 of the display signal, 103.6 in 8 bits;
    // exposure 2 without colour management adds 2/17.52, 132.7
    // (color_measure_render_tests measures the same values on read_frame).
    {
        const std::filesystem::path grey = root / "flat-grey.mp4";
        const std::string generateGrey =
            Quote(FFMPEG_EXECUTABLE) +
            " -hide_banner -loglevel error -f lavfi -i "
            "'color=c=0x686868:s=160x90:r=25:d=2' -c:v libx264 -qp 0 "
            "-pix_fmt yuv420p -colorspace bt709 -color_primaries bt709 "
            "-color_trc bt709 -y " +
            Quote(grey);
        Check(std::system(generateGrey.c_str()) == 0,
              "DEC29: the grey fixture is generated");
        Document graded;
        LibraryMedia flat;
        flat.id = "01K31000000000000000000031";
        flat.path = grey.filename().string();
        flat.filename = flat.path;
        flat.rate = {25, 1};
        flat.duration = {50, 25};
        flat.width = 160;
        flat.height = 90;
        flat.pixel_format = "yuv420p";
        graded.library.push_back(flat);
        graded.sources.push_back(
            {flat.id, flat.path, flat.rate, flat.duration});
        graded.sequence.width = 160;
        graded.sequence.height = 90;
        graded.sequence.frame_rate = {25, 1};
        DocumentClip plain{
            "01K31000000000000000000032", flat.id, {0, 25}, {25, 25}, {0, 25}};
        DocumentClip lifted{"01K31000000000000000000033",
                            flat.id,
                            {25, 25},
                            {25, 25},
                            {25, 25}};
        ClipEffect exposure;
        exposure.type = "color.exposure";
        exposure.params["amount"] = {2, 1};
        lifted.effects = {exposure};
        graded.sequence.tracks = {
            {"01K31000000000000000000034", "video", 0, {plain, lifted}}};

        TimelineSheetSettings gradedSettings;
        gradedSettings.ffmpeg_path = FFMPEG_EXECUTABLE;
        gradedSettings.cell_dimension = 128;
        // The decoded sheet, and its size, as a model's decoder reads it.
        const auto decodeSheet = [&](const std::string& jpeg,
                                     const std::string& name, int& width,
                                     int& height, std::string& raster) {
            { std::ofstream(root / (name + ".jpg"), std::ios::binary) << jpeg; }
            const std::string toPpm = Quote(FFMPEG_EXECUTABLE) +
                                      " -hide_banner -loglevel error -y -i " +
                                      Quote(root / (name + ".jpg")) +
                                      " -c:v ppm " +
                                      Quote(root / (name + ".ppm"));
            if (std::system(toPpm.c_str()) != 0) return false;
            std::ifstream ppm(root / (name + ".ppm"), std::ios::binary);
            std::string magic;
            int maximum = 0;
            ppm >> magic >> width >> height >> maximum;
            ppm.get();
            raster.assign(std::istreambuf_iterator<char>(ppm),
                          std::istreambuf_iterator<char>());
            return magic == "P6" &&
                   raster.size() == static_cast<size_t>(width) * height * 3;
        };
        // The mean red of a 16-pixel square at the centre of a 128 x 72
        // cell.
        const auto cellRed = [&](const std::string& raster, int width,
                                 int cell) {
            const int columns = width / 128;
            if (columns <= 0) return -1.0;
            const int cx = (cell % columns) * 128 + 64;
            const int cy = (cell / columns) * 72 + 36;
            double sum = 0.0;
            for (int y = cy - 8; y < cy + 8; ++y)
                for (int x = cx - 8; x < cx + 8; ++x)
                    sum += static_cast<unsigned char>(
                        raster[(static_cast<size_t>(y) * width + x) * 3]);
            return sum / 256.0;
        };
        TimelineSheetPlan contacts;
        Check(BuildTimelineSheetPlan(graded, TimelineSheetKind::Contact,
                                     gradedSettings, contacts, error) &&
                  contacts.frames.size() == 2,
              "DEC29: the contact plan holds both shots: " + error);
        std::string ungradedJpeg;
        std::string gradedJpeg;
        Check(RenderTimelineSheet(graded, root, contacts, gradedSettings,
                                  ungradedJpeg, error),
              "DEC29: the plain sheet renders: " + error);
        gradedSettings.graded = true;
        Check(RenderTimelineSheet(graded, root, contacts, gradedSettings,
                                  gradedJpeg, error),
              "DEC29: the graded sheet renders: " + error);
        int plainWidth = 0;
        int plainHeight = 0;
        int gradedWidth = 0;
        int gradedHeight = 0;
        std::string plainRaster;
        std::string gradedRaster;
        const bool decoded =
            decodeSheet(ungradedJpeg, "dec29-plain", plainWidth, plainHeight,
                        plainRaster) &&
            decodeSheet(gradedJpeg, "dec29-graded", gradedWidth, gradedHeight,
                        gradedRaster);
        Check(decoded && plainWidth == 256 && plainHeight == 72 &&
                  gradedWidth == plainWidth && gradedHeight == plainHeight,
              "DEC29: the graded sheet has the plain sheet's grid, 256 x 72, "
              "got " +
                  std::to_string(gradedWidth) + " x " +
                  std::to_string(gradedHeight));
        if (decoded && gradedWidth == 256) {
            const double plainFirst = cellRed(plainRaster, plainWidth, 0);
            const double plainSecond = cellRed(plainRaster, plainWidth, 1);
            const double gradedFirst = cellRed(gradedRaster, gradedWidth, 0);
            const double gradedSecond = cellRed(gradedRaster, gradedWidth, 1);
            Check(std::abs(plainFirst - 103.6) < 3.0 &&
                      std::abs(plainSecond - 103.6) < 3.0,
                  "DEC29: the plain sheet shows both shots ungraded: " +
                      std::to_string(plainFirst) + ", " +
                      std::to_string(plainSecond));
            Check(std::abs(gradedFirst - 103.6) < 3.0,
                  "DEC29: an ungraded shot keeps its grey on the graded "
                  "sheet: " +
                      std::to_string(gradedFirst));
            Check(std::abs(gradedSecond - 132.7) < 3.0,
                  "DEC29: the graded shot shows its grade: " +
                      std::to_string(gradedSecond));
        }
        Check(SerializeTimelineSheetPlan(contacts, true)
                          .find("\"graded\":true") != std::string::npos &&
                  SerializeTimelineSheetPlan(contacts).find("graded") ==
                      std::string::npos,
              "DEC29: the metadata says graded only for a graded sheet");

        // The cut between them: the last grey frame before, the first
        // lifted one after, each with its own grade.
        TimelineSheetPlan cut;
        std::string cutJpeg;
        int cutWidth = 0;
        int cutHeight = 0;
        std::string cutRaster;
        Check(BuildTimelineSheetPlan(graded, TimelineSheetKind::Cuts,
                                     gradedSettings, cut, error) &&
                  cut.frames.size() == 2 &&
                  RenderTimelineSheet(graded, root, cut, gradedSettings,
                                      cutJpeg, error) &&
                  decodeSheet(cutJpeg, "dec29-cut", cutWidth, cutHeight,
                              cutRaster) &&
                  cutWidth == 256,
              "DEC29: the graded cut sheet renders its pair: " + error);
        if (cutWidth == 256 && !cutRaster.empty()) {
            Check(std::abs(cellRed(cutRaster, cutWidth, 0) - 103.6) < 3.0 &&
                      std::abs(cellRed(cutRaster, cutWidth, 1) - 132.7) < 3.0,
                  "DEC29: each side of the cut carries its own clip's grade");
        }

        // A sheet of rushes is never graded.
        TimelineSheetPlan rushPlan;
        TimelineSheetSettings rushSettings = gradedSettings;
        std::string rushJpeg;
        Check(BuildMediaSheetPlan(graded, {flat.id}, rushSettings, rushPlan,
                                  error) &&
                  !RenderTimelineSheet(graded, root, rushPlan, rushSettings,
                                       rushJpeg, error) &&
                  error.find("rush has no grade") != std::string::npos,
              "DEC29: a graded sheet of rushes is refused by name: " + error);
    }

    std::filesystem::remove_all(root);
    if (failures == 0) std::cout << "timeline sheet tests passed\n";
    return failures == 0 ? 0 : 1;
}
