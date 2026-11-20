// REMOTION-PARITY-2026-09 REM1 -- ROADMAP.md. This corpus is CUTMACHINE's
// executable creative contract: ten independent scenes, exact control-frame
// descriptors, deterministic evaluation and named refusals. It tests our
// data model, not Remotion's implementation.

#include "MotionAudio.h"
#include "MotionComposition.h"
#include "MotionEffects.h"
#include "MotionEvaluate.h"
#include "MotionGeometry.h"
#include "golden_image.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "TestSupport.h"

namespace {

bool Check2(bool condition, const std::string& label,
            const MotionError& error) {
    Check(condition, label + ": " + error.code + " at " + error.path + " -- " +
                         error.message);
    return condition;
}

bool Near(double left, double right, double tolerance = 1e-6) {
    return std::fabs(left - right) <= tolerance;
}

class ParityMeasurer final : public MotionMeasurer {
public:
    bool LayoutText(const std::string& text, const MotionTextStyle& style,
                    double max_width, MotionTextLayout& layout,
                    MotionError& error) override {
        if (style.font_family == "Missing Sans") {
            error = {"FontMissing", "", "font 'Missing Sans' is not installed"};
            return false;
        }
        const double lineHeight =
            style.line_height > 0.0 ? style.line_height : style.font_size * 1.2;
        const double advance = style.font_size * 0.5 + style.letter_spacing;
        layout = MotionTextLayout{};
        std::stringstream stream(text);
        std::string line;
        std::vector<std::pair<std::string, double>> lines;
        std::vector<std::vector<MotionTextCluster>> clusters;
        while (std::getline(stream, line, '\n')) {
            clusters.emplace_back();
            double x = 0.0;
            for (size_t index = 0; index < line.size(); ++index) {
                if ((static_cast<unsigned char>(line[index]) & 0xC0) == 0x80) {
                    clusters.back().back().length += 1;
                    continue;
                }
                const double width =
                    advance + (line[index] == ' ' ? style.word_spacing : 0.0);
                clusters.back().push_back({index, 1, x, width});
                x += width;
            }
            lines.emplace_back(line, x);
            layout.width = std::max(layout.width, x);
        }
        const double frame = max_width >= 0.0 ? max_width : layout.width;
        for (size_t index = 0; index < lines.size(); ++index) {
            MotionTextLine placed;
            placed.text = lines[index].first;
            placed.clusters = clusters[index];
            placed.width = lines[index].second;
            placed.height = lineHeight;
            placed.y = lineHeight * static_cast<double>(index);
            placed.x = style.text_align == "center"
                           ? (frame - placed.width) / 2.0
                       : style.text_align == "right" ? frame - placed.width
                                                     : 0.0;
            placed.baseline = placed.y + style.font_size * 0.8;
            layout.lines.push_back(placed);
        }
        layout.height = lineHeight * static_cast<double>(lines.size());
        return true;
    }

    bool ImageSize(const std::string& source, double& width, double& height,
                   MotionError& error) override {
        if (!logo_available || source != "logo.png") {
            error = {"MediaMissing", "", "no media '" + source + "'"};
            return false;
        }
        width = 400.0;
        height = 200.0;
        return true;
    }

    // REM2 -- three media: a picture without sound at the NTSC cadence, a
    // picture with its sound at 25 fps, and a sound alone.
    bool MediaInfo(const std::string& src, const std::string& media,
                   MotionMediaInfo& info, MotionError& error) override {
        info = MotionMediaInfo{};
        if (src == "clip.mov") {
            info.has_video = true;
            info.width = 1920;
            info.height = 1080;
            info.rate_numerator = 30000;
            info.rate_denominator = 1001;
            info.duration = {300300, 30000};  // 300 pictures
            return true;
        }
        if (src == "voice.mov" || media == kVoiceMedia) {
            info.has_video = true;
            info.has_audio = true;
            info.width = 640;
            info.height = 360;
            info.rate_numerator = 25;
            info.rate_denominator = 1;
            info.duration = {100, 25};  // four seconds
            return true;
        }
        if (src == "music.wav") {
            info.has_audio = true;
            info.duration = {144000, 48000};
            return true;
        }
        error = {"MediaMissing", "",
                 "no media '" + (media.empty() ? src : media) + "'"};
        return false;
    }

    // voice.mov: four seconds of a sawtooth, left n % 1000 / 1000, right
    // its opposite, so that every sample says where it was read from.
    bool MediaSound(const std::string& src, const std::string& media,
                    std::shared_ptr<const MotionPcm>& pcm,
                    MotionError& error) override {
        MotionMediaInfo info;
        if (!MediaInfo(src, media, info, error)) {
            return false;
        }
        auto sound = std::make_shared<MotionPcm>();
        for (int64_t n = 0; n < 4 * 48000; ++n) {
            sound->left.push_back(static_cast<float>(n % 1000) / 1000.0f);
            sound->right.push_back(-sound->left.back());
        }
        pcm = sound;
        return true;
    }

    static constexpr const char* kVoiceMedia = "01K7C000000000000000001399";
    bool logo_available = true;
};

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream input(path);
    std::ostringstream contents;
    contents << input.rdbuf();
    Check(input.good() || input.eof(), "fixture is readable: " + path.string());
    return contents.str();
}

mcp_json::Value Json(const std::string& text) {
    mcp_json::Value value;
    std::string error;
    Check(mcp_json::Value::Parse(text, value, error),
          "test JSON parses: " + error);
    return value;
}

MotionComposition Load(const std::string& name) {
    const std::filesystem::path path =
        std::filesystem::path(MOTION_PARITY_FIXTURE_DIR) / name;
    MotionComposition composition;
    MotionError error;
    Check(ParseMotionComposition(ReadFile(path), composition, error),
          name + " parses: " + error.code + " at " + error.path + " -- " +
              error.message);
    return composition;
}

bool Evaluate(const MotionComposition& composition, int64_t frame,
              MotionFrame& output, const mcp_json::Value& props = Json("null"),
              MotionError* failure = nullptr,
              ParityMeasurer* supplied_measurer = nullptr) {
    ParityMeasurer localMeasurer;
    MotionError error;
    const bool ok = EvaluateMotionFrame(
        composition, props, frame,
        supplied_measurer != nullptr ? *supplied_measurer : localMeasurer,
        output, error);
    if (failure != nullptr) {
        *failure = error;
    } else {
        Check(ok, "frame " + std::to_string(frame) +
                      " evaluates: " + error.code + " at " + error.path +
                      " -- " + error.message);
    }
    return ok;
}

bool Resolve(const MotionComposition& composition, const std::string& variant,
             const std::string& format, MotionComposition& resolved,
             mcp_json::Value& props, MotionError* failure = nullptr) {
    MotionError error;
    const bool ok = ResolveMotionVariant(composition, variant, format,
                                         Json("null"), resolved, props, error);
    if (failure != nullptr) {
        *failure = error;
    } else {
        Check(ok, "variant '" + variant + "' in format '" + format +
                      "' resolves: " + error.code + " -- " + error.message);
    }
    return ok;
}

const MotionEvaluatedNode* FindId(const MotionEvaluatedNode& node,
                                  const std::string& id) {
    if (node.id == id) {
        return &node;
    }
    for (const MotionEvaluatedNode& child : node.children) {
        if (const MotionEvaluatedNode* found = FindId(child, id)) {
            return found;
        }
    }
    return nullptr;
}

bool BoxIs(const MotionEvaluatedNode* node, double x, double y, double width,
           double height) {
    return node != nullptr && Near(node->box.x, x, 1e-3) &&
           Near(node->box.y, y, 1e-3) && Near(node->box.width, width, 1e-3) &&
           Near(node->box.height, height, 1e-3);
}

void CheckDeterminism(const std::string& fixture,
                      const std::vector<int64_t>& frames) {
    const MotionComposition composition = Load(fixture);
    for (int64_t frame : frames) {
        MotionFrame first;
        MotionFrame second;
        if (Evaluate(composition, frame, first) &&
            Evaluate(composition, frame, second)) {
            Check(DumpMotionFrame(first) == DumpMotionFrame(second),
                  fixture + " frame " + std::to_string(frame) +
                      " is byte-deterministic");
        }
    }
}

void TestC01CenteredCard() {
    const MotionComposition composition = Load("c01-centered-card.json");
    struct Expected {
        const char* format;
        int width;
        int height;
        double x;
        double y;
    };
    for (const Expected& expected : {
             Expected{"9x16", 1080, 1920, 470.0, 912.0},
             Expected{"16x9", 1920, 1080, 890.0, 492.0},
         }) {
        MotionComposition resolved;
        mcp_json::Value props;
        MotionFrame frame;
        if (Resolve(composition, "", expected.format, resolved, props) &&
            Evaluate(resolved, 12, frame, props)) {
            const MotionEvaluatedNode* title = FindId(frame.root, "title");
            Check(frame.width == expected.width &&
                      frame.height == expected.height &&
                      BoxIs(title, expected.x, expected.y, 140, 96) &&
                      title->uid == "01K71000000000000000000002" &&
                      title->text_layout.lines.size() == 2,
                  std::string("C01 centers the same two-line card in ") +
                      expected.format + ": " + DumpMotionFrame(frame));
        }
    }
}

void TestC02StaggeredCredit() {
    const MotionComposition composition = Load("c02-staggered-credit.json");
    MotionFrame at0;
    MotionFrame at3;
    MotionFrame at9;
    if (Evaluate(composition, 0, at0) && Evaluate(composition, 3, at3) &&
        Evaluate(composition, 9, at9)) {
        const MotionEvaluatedNode* name0 = FindId(at0.root, "name");
        const MotionEvaluatedNode* role0 = FindId(at0.root, "role");
        const MotionEvaluatedNode* name3 = FindId(at3.root, "name");
        const MotionEvaluatedNode* role3 = FindId(at3.root, "role");
        const MotionEvaluatedNode* name9 = FindId(at9.root, "name");
        const MotionEvaluatedNode* role9 = FindId(at9.root, "role");
        Check(BoxIs(name0, 80, 896.4, 270, 72) &&
                  BoxIs(role0, 80, 980.4, 270, 43.2) &&
                  Near(name0->opacity, 0) && Near(role0->opacity, 0) &&
                  Near(name0->translate_x, -80) &&
                  Near(role0->translate_x, -80),
              "C02 fixes both credit boxes outside and transparent: " +
                  DumpMotionFrame(at0));
        Check(name3 != nullptr && role3 != nullptr &&
                  Near(name3->opacity, 0.5) && Near(role3->opacity, 0) &&
                  Near(role3->translate_x, -80),
              "C02 keeps the role delayed while the name enters");
        Check(name9 != nullptr && role9 != nullptr && Near(name9->opacity, 1) &&
                  Near(role9->opacity, 0.5) &&
                  name9->uid == "01K72000000000000000000003",
              "C02 exposes the stagger and stable name identity at frame 9");
    }
}

void TestC03WordCascade() {
    const MotionComposition composition = Load("c03-word-cascade.json");
    MotionFrame frame;
    if (Evaluate(composition, 4, frame)) {
        const MotionEvaluatedNode* words = FindId(frame.root, "words");
        Check(BoxIs(words, 180, 201, 440, 48) && words->words.size() == 3 &&
                  Near(words->words[0].x, 0) && Near(words->words[1].x, 140) &&
                  Near(words->words[2].x, 240),
              "C03 fixes the laid-out word boxes at its control frame");
        Check(words != nullptr && words->units.size() == 3 &&
                  Near(words->units[0].opacity, 1) &&
                  Near(words->units[1].opacity, 0) &&
                  Near(words->units[2].opacity, 0.5) &&
                  words->units[0].delay == 0 && words->units[1].delay == 4 &&
                  words->units[2].delay == 2 &&
                  Near(words->units[0].translate_y, 0) &&
                  Near(words->units[1].translate_y, 20) &&
                  Near(words->units[2].translate_y, 10) &&
                  Near(words->units[0].color.red, 1) &&
                  Near(words->units[1].color.red, 0xe4 / 255.0) &&
                  Near(words->units[2].color.red, 0xe4 / 255.0),
              "C03 fixes cascade values and the two-word colour range: " +
                  DumpMotionFrame(frame));
    }
}

void TestC04SeriesAndTimeScale() {
    const MotionComposition composition = Load("c04-series-timescale.json");
    const auto ids = [&](int64_t frameNumber, MotionFrame& frame) {
        std::string present;
        if (Evaluate(composition, frameNumber, frame)) {
            for (const char* id : {"first", "second"}) {
                if (FindId(frame.root, id) != nullptr) {
                    present += id[0];
                }
            }
        }
        return present;
    };
    MotionFrame at9;
    MotionFrame at10;
    MotionFrame at18;
    Check(ids(9, at9) == "fs" && ids(10, at10) == "s" && ids(18, at18) == "s",
          "C04 fixes the overlap boundary of the two-card series");
    const MotionEvaluatedNode* second = FindId(at9.root, "second");
    Check(BoxIs(second, 0, 0, 100, 100) && Near(second->opacity, 0.1) &&
              at9.root.children.size() == 2 &&
              at9.root.children[1].has_local_frame &&
              at9.root.children[1].local_numerator == 1 &&
              at9.root.children[1].local_denominator == 2,
          "C04 keeps 50% local time as the exact fraction 1/2");
}

void TestC05RepeatPalette() {
    const MotionComposition composition = Load("c05-repeat-palette.json");
    MotionFrame frame;
    if (Evaluate(composition, 2, frame)) {
        const MotionEvaluatedNode* list = FindId(frame.root, "list");
        Check(list != nullptr && list->children.size() == 3 &&
                  BoxIs(&list->children[0], 0, 0, 160, 30) &&
                  BoxIs(&list->children[1], 0, 40, 160, 30) &&
                  BoxIs(&list->children[2], 0, 80, 160, 30),
              "C05 lays three repeated rows into the parent flow");
        if (list != nullptr && list->children.size() == 3) {
            const auto& rows = list->children;
            Check(rows[0].id == "row[0]" && rows[1].id == "row[1]" &&
                      rows[2].id == "row[2]" &&
                      rows[0].uid == "01K75000000000000000000004" &&
                      rows[0].children[0].text == "Coupe" &&
                      rows[2].children[0].text == "Regard",
                  "C05 binds list items while preserving template identity");
            Check(rows[0].background.red == 0xe4 / 255.0 &&
                      rows[1].background.green == 0xab / 255.0 &&
                      rows[2].background.blue == 0xfe / 255.0 &&
                      Near(rows[0].opacity, 1) && Near(rows[1].opacity, 0) &&
                      Near(rows[2].opacity, 0),
                  "C05 cycles the palette and staggers each repeated row");
        }
    }
    MotionFrame overridden;
    if (Evaluate(composition, 0, overridden,
                 Json(R"({"items":["Une","Deux"]})"))) {
        const MotionEvaluatedNode* list = FindId(overridden.root, "list");
        Check(list != nullptr && list->children.size() == 2,
              "C05 derives repeat count from an overridden list prop");
    }
}

void TestC06PathTrim() {
    const MotionComposition composition = Load("c06-path-trim.json");
    MotionFrame middle;
    MotionFrame end;
    if (Evaluate(composition, 10, middle) && Evaluate(composition, 20, end)) {
        const MotionEvaluatedNode* half = FindId(middle.root, "mark");
        const MotionEvaluatedNode* full = FindId(end.root, "mark");
        Check(half != nullptr && Near(half->path_paint.length, 200) &&
                  Near(half->path_paint.trim_end, 0.5) &&
                  half->path_paint.has_bounds &&
                  Near(half->path_paint.bounds.x, 20) &&
                  Near(half->path_paint.bounds.y, 30) &&
                  Near(half->path_paint.bounds.width, 100) &&
                  Near(half->path_paint.bounds.height, 0),
              "C06 fixes the half-drawn path length and bounds");
        Check(full != nullptr && Near(full->path_paint.trim_end, 1) &&
                  Near(full->path_paint.bounds.width, 100) &&
                  Near(full->path_paint.bounds.height, 100) &&
                  Near(full->path_paint.stroke_width, 4),
              "C06 fixes the completed path and its stroke");
    }
}

void TestC07PolygonClip() {
    const MotionComposition composition = Load("c07-polygon-clip.json");
    MotionFrame frame;
    if (Evaluate(composition, 5, frame)) {
        const MotionEvaluatedNode* panel = FindId(frame.root, "panel");
        Check(panel != nullptr && BoxIs(panel, 100, 50, 200, 100) &&
                  panel->has_clip && panel->clip.size() == 3 &&
                  Near(panel->clip[0].first, 100) &&
                  Near(panel->clip[0].second, 50) &&
                  Near(panel->clip[1].first, 200) &&
                  Near(panel->clip[1].second, 50) &&
                  Near(panel->clip[2].first, 150) &&
                  Near(panel->clip[2].second, 150),
              "C07 resolves the animated polygon in composition pixels");
    }
}

void TestC08LogoFitAndAlpha() {
    const MotionComposition composition = Load("c08-logo-fit.json");
    MotionFrame frame;
    if (Evaluate(composition, 0, frame)) {
        const MotionEvaluatedNode* contain = FindId(frame.root, "contain");
        const MotionEvaluatedNode* cover = FindId(frame.root, "cover");
        Check(BoxIs(contain, 80, 105, 240, 240) &&
                  BoxIs(cover, 480, 105, 240, 240) &&
                  contain->image_source == "logo.png" &&
                  contain->object_fit == "contain" &&
                  cover->object_fit == "cover" && Near(cover->opacity, 0.5),
              "C08 fixes contain, cover and alpha in two equal logo boxes: " +
                  DumpMotionFrame(frame));
    }
    MotionComposition missingComposition;
    MotionError parseError;
    Check(
        ParseMotionComposition(
            R"({"width":100,"height":100,"fps":25,"duration":1,"root":{"type":"fill","children":[{"type":"image","src":"absent.png"}]}})",
            missingComposition, parseError),
        "C08 missing-media control parses");
    MotionFrame absent;
    MotionError error;
    Check(!Evaluate(missingComposition, 0, absent, Json("null"), &error) &&
              error.code == "MediaMissing",
          "C08 refuses a missing logo as MediaMissing");
}

void TestC09FontControl() {
    const MotionComposition composition = Load("c09-font-control.json");
    MotionFrame normal;
    MotionFrame large;
    if (Evaluate(composition, 0, normal) &&
        Evaluate(composition, 0, large, Json(R"({"size":80})"))) {
        const MotionEvaluatedNode* normalText = FindId(normal.root, "sample");
        const MotionEvaluatedNode* largeText = FindId(large.root, "sample");
        Check(BoxIs(normalText, 210, 156, 220, 48) &&
                  BoxIs(largeText, 100, 132, 440, 96) &&
                  Near(normalText->text_style.font_size, 40) &&
                  Near(largeText->text_style.font_size, 80),
              "C09 makes font size an exact, bounded layout control");
    }
    MotionFrame missing;
    MotionError error;
    Check(!Evaluate(composition, 0, missing, Json(R"({"font":"Missing Sans"})"),
                    &error) &&
              error.code == "FontMissing",
          "C09 refuses an unavailable selected font as FontMissing");
    Check(!Evaluate(composition, 0, missing, Json(R"({"size":81})"), &error) &&
              error.code == "PropOutOfRange",
          "C09 refuses a font size outside its declared range");
}

void TestC10VariantsAndFormats() {
    const MotionComposition composition = Load("c10-variants-formats.json");
    const std::vector<std::string> variants = {"alpha", "beta", "gamma"};
    const std::vector<std::string> titles = {"Version A", "Version B",
                                             "Version C"};
    struct Format {
        const char* name;
        int width;
        int height;
        double size;
        double x;
        double y;
    };
    const Format formats[] = {{"9x16", 1080, 1920, 60, 405, 924},
                              {"16x9", 1920, 1080, 80, 780, 492},
                              {"1x1", 1080, 1080, 70, 382.5, 498}};
    int combinations = 0;
    for (size_t variant = 0; variant < variants.size(); ++variant) {
        for (const Format& format : formats) {
            MotionComposition resolved;
            mcp_json::Value props;
            MotionFrame first;
            MotionFrame second;
            if (Resolve(composition, variants[variant], format.name, resolved,
                        props) &&
                Evaluate(resolved, 12, first, props) &&
                Evaluate(resolved, 12, second, props)) {
                const MotionEvaluatedNode* title = FindId(first.root, "title");
                Check(
                    first.width == format.width &&
                        first.height == format.height && title != nullptr &&
                        title->text == titles[variant] &&
                        Near(title->text_style.font_size, format.size) &&
                        BoxIs(title, format.x, format.y, format.size * 4.5,
                              format.size * 1.2) &&
                        title->uid == "01K7A000000000000000000002" &&
                        DumpMotionFrame(first) == DumpMotionFrame(second),
                    "C10 resolves " + variants[variant] + " x " + format.name);
                ++combinations;
            }
        }
    }
    Check(combinations == 9, "C10 evaluates all 3 x 3 deliveries");

    MotionComposition resolved;
    mcp_json::Value props;
    MotionError error;
    Check(!Resolve(composition, "delta", "9x16", resolved, props, &error) &&
              error.code == "UnknownVariant",
          "C10 refuses an unknown version as UnknownVariant");
    Check(!Resolve(composition, "alpha", "4x5", resolved, props, &error) &&
              error.code == "UnknownFormat",
          "C10 refuses an unknown format as UnknownFormat");
}

// REMOTION-PARITY-2026-09 REM2 -- C12: a video cropped in a rounded box.
// Each composition frame names the source picture it shows, by the exact
// media time (startFrom + t * playbackRate) / fps and the media's own
// cadence -- here 30000/1001 under a 25 fps composition at half speed.
void TestC12RoundedVideo() {
    const MotionComposition composition = Load("c12-rounded-video.json");
    struct Expected {
        int64_t frame;
        const char* state;
        int64_t numerator;
        int64_t denominator;
        int64_t picture;
    };
    // Media time at composition frame f: (10 + f / 2) / 25 seconds; the
    // picture shown is floor(time * 30000 / 1001).
    for (const Expected& expected : {
             Expected{0, "playing", 2, 5, 11},
             Expected{1, "playing", 21, 50, 12},
             Expected{25, "playing", 9, 10, 26},
             Expected{39, "playing", 59, 50, 35},
             Expected{40, "after", 6, 5, 0},
         }) {
        MotionFrame frame;
        if (!Evaluate(composition, expected.frame, frame)) {
            continue;
        }
        const MotionEvaluatedNode* clip = FindId(frame.root, "clip");
        const MotionEvaluatedNode* box = FindId(frame.root, "frame");
        Check(
            clip != nullptr && clip->type == MotionNodeType::Video &&
                BoxIs(clip, 160, 90, 320, 180) && clip->object_fit == "cover" &&
                clip->media.muted && clip->media.src == "clip.mov" &&
                clip->media.state == expected.state &&
                clip->media.time_numerator == expected.numerator &&
                clip->media.time_denominator == expected.denominator &&
                clip->media.frame == expected.picture &&
                clip->uid == "01K7C000000000000000001203",
            "C12 frame " + std::to_string(expected.frame) +
                " names its source picture exactly: " + DumpMotionFrame(frame));
        Check(box != nullptr && box->overflow_hidden &&
                  Near(box->border_radius, 24) && BoxIs(box, 160, 90, 320, 180),
              "C12 crops the video in a rounded box that hides its overflow");
    }
    MotionComposition missing = composition;
    missing.root.children[0].children[0].src->literal =
        mcp_json::Value::MakeString("absent.mov");
    MotionFrame frame;
    MotionError error;
    Check(!Evaluate(missing, 0, frame, Json("null"), &error) &&
              error.code == "MediaMissing" &&
              error.path == "root.children[0].children[0].src",
          "C12 refuses an absent media by name, where it is named: " +
              error.code + " at " + error.path);
    MotionComposition silent = composition;
    silent.root.children[0].children[0].src->literal =
        mcp_json::Value::MakeString("music.wav");
    Check(!Evaluate(silent, 0, frame, Json("null"), &error) &&
              error.code == "MediaHasNoVideo",
          "C12 refuses a sound alone as a video's media");
}

// REM2 -- C13: a video whose sound is detached, with a gain and fades. The
// picture and the sound read the same instant of the same media, and the
// mix is checked sample by sample before any codec.
void TestC13DetachedSound() {
    const MotionComposition composition = Load("c13-detached-sound.json");
    ParityMeasurer measurer;
    // The picture shows media time f / 25 at every frame, the sound reads
    // the sample of the same instant: 10 + (f - 10) frames in.
    MotionFrame at30;
    if (Evaluate(composition, 30, at30)) {
        const MotionEvaluatedNode* picture = FindId(at30.root, "picture");
        const MotionEvaluatedNode* sound = FindId(at30.root, "sound");
        Check(picture != nullptr && sound != nullptr &&
                  picture->media.time_numerator == 6 &&
                  picture->media.time_denominator == 5 &&
                  sound->media.time_numerator == 6 &&
                  sound->media.time_denominator == 5 &&
                  Near(sound->media.volume, 0.5) && BoxIs(sound, 0, 0, 0, 0),
              "C13 plays picture and sound at the same media instant: " +
                  DumpMotionFrame(at30));
    }
    std::string voices;
    size_t audible = 0;
    MotionError error;
    Check(DescribeMotionAudio(composition, Json("null"),
                              MotionEvaluationContext{}, measurer, voices,
                              audible, error) &&
              audible == 1 &&
              voices ==
                  "{\"samples\":144000,\"rate\":48000,\"voices\":[{\"path\":"
                  "\"root.children[1].children[0]\",\"uid\":"
                  "\"01K7C000000000000000001304\",\"type\":\"audio\","
                  "\"src\":\"voice.mov\",\"first\":19200,\"end\":115200,"
                  "\"mediaSample\":19200,\"volume\":\"animated\"}]}",
          "C13 describes its one voice: heard from sample 19200 to 115200, "
          "reading the media from the same sample: " +
              voices + " " + error.message);
    MotionPcm mix;
    Check(MixMotionAudio(composition, Json("null"), MotionEvaluationContext{},
                         measurer, 0, MotionAudioSampleCount(composition), mix,
                         error),
          "C13 mixes: " + error.code + " " + error.message);
    const auto sample = [](int64_t n) {
        return static_cast<float>(n % 1000) / 1000.0f;
    };
    struct Point {
        int64_t n;
        double gain;
    };
    // Local frame l = n / 1920 - 10; the fade is 0 -> 0.5 over l in [0, 5]
    // and 0.5 -> 0 over [45, 50].
    for (const Point& point :
         {Point{19199, 0.0}, Point{19200, 0.0}, Point{24000, 0.25},
          Point{28800, 0.5}, Point{50000, 0.5}, Point{110400, 0.25},
          Point{115199, 0.5 * 1.0 / 9600.0}, Point{115200, 0.0}}) {
        const float expected = static_cast<float>(sample(point.n) * point.gain);
        Check(mix.left.size() == 144000 &&
                  std::fabs(mix.left[point.n] - expected) <= 1e-6 &&
                  std::fabs(mix.right[point.n] + expected) <= 1e-6,
              "C13 sample " + std::to_string(point.n) +
                  " is the media's own "
                  "sample times its gain " +
                  std::to_string(point.gain) + ": " +
                  (mix.left.size() > static_cast<size_t>(point.n)
                       ? std::to_string(mix.left[point.n])
                       : std::string("missing")));
    }
    MotionPcm window;
    Check(MixMotionAudio(composition, Json("null"), MotionEvaluationContext{},
                         measurer, 24000, 10, window, error) &&
              window.left.size() == 10 && window.left[0] == mix.left[24000] &&
              window.left[9] == mix.left[24009],
          "C13 mixes a window with the same samples as the whole");
    // A sound at another speed is refused, never resampled.
    MotionComposition fast = composition;
    fast.root.children[0].muted = false;
    fast.root.children[0].playback_rate =
        MotionValue{MotionBindingKind::Literal, Json("2")};
    Check(!MixMotionAudio(fast, Json("null"), MotionEvaluationContext{},
                          measurer, 0, 10, mix, error) &&
              error.code == "UnsupportedAudioRate" &&
              error.path == "root.children[0]",
          "C13 refuses a sound that would play at another speed: " +
              error.code + " at " + error.path);
}

// REM2 -- C14: one precomposition shown twice with its own props. Each
// instance evaluates the lower third at its own time and cadence (50 fps
// under 25), keeps the lower third's identities, and a cycle is refused.
void TestC14Precomposition() {
    const MotionComposition parent = Load("c14-precomposition.json");
    MotionLibrary library;
    library.compositions.emplace("01K7C000000000000000001400",
                                 Load("c14-lower-third.json"));
    MotionEvaluationContext context;
    context.library = &library;
    context.composition_id = "01K7C000000000000000001410";
    ParityMeasurer measurer;
    const auto evaluate = [&](int64_t frame, MotionFrame& output,
                              MotionError& error) {
        return EvaluateMotionFrame(parent, Json("null"), frame, measurer,
                                   context, output, error);
    };
    MotionFrame at5;
    MotionFrame at17;
    MotionFrame at50;
    MotionError error;
    if (!Check2(evaluate(5, at5, error), "C14 frame 5", error) ||
        !Check2(evaluate(17, at17, error), "C14 frame 17", error) ||
        !Check2(evaluate(50, at50, error), "C14 frame 50", error)) {
        return;
    }
    const MotionEvaluatedNode* left5 = FindId(at5.root, "left");
    Check(left5 != nullptr && BoxIs(left5, 40, 40, 240, 90) &&
              left5->instance.composition_id == "01K7C000000000000000001400" &&
              left5->instance.state == "playing" &&
              left5->instance.local_numerator == 10 &&
              left5->instance.local_denominator == 1 &&
              Near(left5->instance.scale_x, 0.5) &&
              Near(left5->instance.origin_x, 40) &&
              left5->instance_root.size() == 1 &&
              FindId(at5.root, "right") == nullptr,
          "C14 shows the left instance at its own frame 10, half size, the "
          "right one not yet: " +
              DumpMotionFrame(at5));
    if (left5 != nullptr && left5->instance_root.size() == 1) {
        const MotionEvaluatedNode* band =
            FindId(left5->instance_root[0], "band");
        const MotionEvaluatedNode* label =
            FindId(left5->instance_root[0], "label");
        Check(band != nullptr && Near(band->opacity, 1) &&
                  Near(band->background.red, 0xe4 / 255.0) &&
                  band->uid == "01K7C000000000000000001402" &&
                  label != nullptr && label->text == "Ana",
              "C14 gives the left instance its props, one bound to the "
              "parent's accent, and the lower third's identities");
    }
    const MotionEvaluatedNode* left17 = FindId(at17.root, "left");
    const MotionEvaluatedNode* right17 = FindId(at17.root, "right");
    Check(left17 != nullptr && right17 != nullptr &&
              left17->instance.local_numerator == 34 &&
              right17->instance.local_numerator == 10 &&
              BoxIs(right17, 360, 200, 240, 90) &&
              right17->instance_root.size() == 1 &&
              FindId(right17->instance_root[0], "label")->text == "Bruno" &&
              Near(FindId(right17->instance_root[0], "band")->background.green,
                   0xab / 255.0) &&
              right17->uid == "01K7C000000000000000001414",
          "C14 shows the same lower third twice, each at its own time with "
          "its own props: " +
              DumpMotionFrame(at17));
    const MotionEvaluatedNode* left50 = FindId(at50.root, "left");
    const MotionEvaluatedNode* right50 = FindId(at50.root, "right");
    Check(left50 != nullptr && left50->instance.state == "after" &&
              left50->instance_root.empty() && right50 != nullptr &&
              right50->instance.state == "playing" &&
              right50->instance.local_numerator == 76,
          "C14 ends the first instance with its composition, not the second");

    // Refusals: no project to find it in, a cycle, and props the lower
    // third does not accept.
    MotionFrame frame;
    Check(
        !EvaluateMotionFrame(parent, Json("null"), 0, measurer, frame, error) &&
            error.code == "UnknownComposition",
        "C14 refuses an instance evaluated without its project");
    MotionLibrary cyclic = library;
    MotionComposition& child = cyclic.compositions.begin()->second;
    MotionNode loop;
    loop.type = MotionNodeType::Composition;
    loop.instance_of = "01K7C000000000000000001410";
    child.root.children.push_back(loop);
    cyclic.compositions.emplace("01K7C000000000000000001410", parent);
    MotionEvaluationContext cycle = context;
    cycle.library = &cyclic;
    Check(!EvaluateMotionFrame(parent, Json("null"), 5, measurer, cycle, frame,
                               error) &&
              error.code == "CompositionCycle",
          "C14 refuses a lower third that shows its own parent: " + error.code);
    MotionComposition wrong = parent;
    wrong.root.children[0].instance_props.emplace_back(
        "nom", MotionValue{MotionBindingKind::Literal, Json("\"x\"")});
    Check(!EvaluateMotionFrame(wrong, Json("null"), 5, measurer, context, frame,
                               error) &&
              error.code == "UnknownProp" &&
              error.path == "root.children[0].props.nom",
          "C14 refuses a prop the lower third does not declare");
    wrong = parent;
    wrong.root.children[0].instance_props[0].second.literal = Json("12");
    Check(!EvaluateMotionFrame(wrong, Json("null"), 5, measurer, context, frame,
                               error) &&
              error.code == "PropTypeMismatch",
          "C14 refuses a name that is not text");
}

// REMOTION-PARITY-2026-09 REM3 -- C15: three scenes, a fade then a slide.
// The series lasts the sum of its scenes less its transitions; during each
// overlap the descriptor gives the progress and how each scene is shown.
void TestC15FadeThenSlide() {
    const MotionComposition composition = Load("c15-fade-slide.json");
    Check(composition.duration == 70 &&
              composition.root.children[2].from == 20 &&
              composition.root.children[4].from == 40 &&
              composition.root.children[1].from == 20 &&
              composition.root.children[3].from == 40,
          "C15 overlaps each pair by its transition: 90 frames of scenes, "
          "70 of series");
    MotionFrame at19;
    MotionFrame at25;
    MotionFrame at30;
    MotionFrame at45;
    if (!Evaluate(composition, 19, at19) || !Evaluate(composition, 25, at25) ||
        !Evaluate(composition, 30, at30) || !Evaluate(composition, 45, at45)) {
        return;
    }
    Check(FindId(at19.root, "first") != nullptr &&
              FindId(at19.root, "second") == nullptr &&
              !at19.root.transition.active,
          "C15 shows the first scene alone before the fade");
    const MotionEvaluatedNode* exiting = FindId(at25.root, "first");
    const MotionEvaluatedNode* entering = FindId(at25.root, "second");
    Check(at25.root.transition.active &&
              at25.root.transition.presentation == MotionPresentation::Fade &&
              at25.root.transition.from == 20 &&
              at25.root.transition.to == 30 &&
              Near(at25.root.transition.progress, 0.5) &&
              at25.root.transition.uid == "01K7C000000000000000001504" &&
              exiting != nullptr && entering != nullptr &&
              exiting->transition_role == "exiting" &&
              entering->transition_role == "entering" &&
              Near(exiting->opacity, 0.5) && Near(entering->opacity, 0.5) &&
              entering->blend_plus_lighter && !exiting->blend_plus_lighter,
          "C15 mixes the two scenes half and half at frame 25: " +
              DumpMotionFrame(at25));
    Check(FindId(at30.root, "first") == nullptr &&
              FindId(at30.root, "second") != nullptr &&
              FindId(at30.root, "second")->transition_role.empty() &&
              Near(FindId(at30.root, "second")->opacity, 1),
          "C15 shows the second scene alone once the fade is over");
    const MotionEvaluatedNode* pushed = FindId(at45.root, "second");
    const MotionEvaluatedNode* arriving = FindId(at45.root, "third");
    Check(at45.root.transition.presentation == MotionPresentation::Slide &&
              Near(at45.root.transition.progress, 0.5) && pushed != nullptr &&
              arriving != nullptr && Near(pushed->translate_x, -160) &&
              Near(arriving->translate_x, 160) && Near(pushed->opacity, 1),
          "C15 slides the third scene in from the right, pushing the "
          "second out, half way at frame 45: " +
              DumpMotionFrame(at45));
    // Refusals: a transition at an end, two in a row, one longer than a
    // scene, one outside a series, a direction on a fade.
    const auto refused = [](const std::string& json, const std::string& code) {
        MotionComposition parsed;
        MotionError error;
        return !ParseMotionComposition(json, parsed, error) &&
               error.code == code;
    };
    const std::string head =
        R"J({"width":10,"height":10,"fps":25,"duration":20,"root":{"type":"series","children":[)J";
    const std::string sequence = R"J({"type":"sequence","duration":10})J";
    const std::string fade =
        R"J({"type":"transition","presentation":"fade","timing":{"duration":4}})J";
    Check(
        refused(head + fade + "," + sequence + "]}}", "InvalidValue") &&
            refused(head + sequence + "," + fade + "," + fade + "," + sequence +
                        "]}}",
                    "InvalidValue") &&
            refused(
                head + sequence +
                    R"J(,{"type":"transition","presentation":"fade","timing":{"duration":12}},)J" +
                    sequence + "]}}",
                "InvalidValue") &&
            refused(
                R"J({"width":10,"height":10,"fps":25,"duration":20,"root":{"type":"fill","children":[)J" +
                    fade + "]}}",
                "InvalidValue") &&
            refused(
                head + sequence +
                    R"J(,{"type":"transition","presentation":"fade","direction":"from-left","timing":{"duration":4}},)J" +
                    sequence + "]}}",
                "InvalidValue"),
        "C15 refuses a transition out of place, too long, or with a "
        "direction a fade does not have");
}

// REM3 -- C16: a gradient under a title, turning and changing colours.
void TestC16AnimatedGradient() {
    const MotionComposition composition = Load("c16-animated-gradient.json");
    MotionFrame at0;
    MotionFrame at10;
    MotionFrame at20;
    if (!Evaluate(composition, 0, at0) || !Evaluate(composition, 10, at10) ||
        !Evaluate(composition, 20, at20)) {
        return;
    }
    const MotionResolvedGradient& first = at0.root.gradient;
    Check(at0.root.has_gradient && first.kind == MotionGradientKind::Linear &&
              Near(first.x0, 0) && Near(first.y0, 90) && Near(first.x1, 320) &&
              Near(first.y1, 90) && first.stops.size() == 2 &&
              Near(first.stops[0].second.red, 0xe4 / 255.0) &&
              Near(first.stops[1].second.blue, 0xfe / 255.0),
          "C16 resolves the 90deg gradient across the frame: " +
              DumpMotionFrame(at0));
    const MotionResolvedGradient& middle = at10.root.gradient;
    Check(Near(middle.x0, 35, 1e-6) && Near(middle.y0, -35, 1e-6) &&
              Near(middle.x1, 285, 1e-6) && Near(middle.y1, 215, 1e-6) &&
              Near(middle.stops[0].second.red, (0xe4 + 0x4d) / 2.0 / 255.0) &&
              Near(middle.stops[0].second.green, (0x57 + 0xab) / 2.0 / 255.0) &&
              Near(middle.stops[1].second.blue, (0xfe + 0x3f) / 2.0 / 255.0),
          "C16 turns the line to 135deg and mixes each stop at frame 10: " +
              DumpMotionFrame(at10));
    Check(Near(at20.root.gradient.x0, 160) && Near(at20.root.gradient.y0, 0) &&
              Near(at20.root.gradient.y1, 180),
          "C16 ends on the vertical gradient");
    const MotionEvaluatedNode* spot = FindId(at0.root, "spot");
    Check(spot != nullptr && spot->has_gradient &&
              spot->gradient.kind == MotionGradientKind::Radial &&
              Near(spot->gradient.x0, 50) && Near(spot->gradient.y0, 30) &&
              Near(spot->gradient.x1, 20) && Near(spot->gradient.y1, 20) &&
              Near(spot->gradient.stops[1].second.alpha, 0),
          "C16 resolves a closest-side circle in its own box");
    const auto refused = [](const std::string& value) {
        MotionComposition parsed;
        MotionError error;
        const std::string json =
            R"J({"width":10,"height":10,"fps":25,"duration":5,"root":{"type":"fill","animate":{"backgroundImage":{"interpolate":{"frames":[0,4],"values":["linear-gradient(90deg, #000 0%, #fff 100%)",")J" +
            value + R"J("]}}}}})J";
        return !ParseMotionComposition(json, parsed, error) &&
               error.code == "InvalidAnimation";
    };
    Check(refused("linear-gradient(90deg, #000, #888, #fff)") &&
              refused("radial-gradient(#000, #fff)"),
          "C16 refuses to mix gradients of other shapes");
}

// REM3 -- C17: a title with a shadow, then a blur, then a bounded glow.
// The descriptor gives the stack at each instant and the pixels it may
// write.
void TestC17EffectStack() {
    const MotionComposition composition = Load("c17-shadow-blur-glow.json");
    MotionFrame at0;
    MotionFrame at10;
    MotionFrame at20;
    if (!Evaluate(composition, 0, at0) || !Evaluate(composition, 10, at10) ||
        !Evaluate(composition, 20, at20)) {
        return;
    }
    const MotionEvaluatedNode* title0 = FindId(at0.root, "title");
    const MotionEvaluatedNode* title10 = FindId(at10.root, "title");
    const MotionEvaluatedNode* title20 = FindId(at20.root, "title");
    Check(title0 != nullptr && title0->filters.size() == 3 &&
              title0->filters[0].kind == MotionFilterKind::DropShadow &&
              Near(title0->filters[0].offset_y, 6) &&
              Near(title0->filters[0].sigma, 4) &&
              Near(title0->filters[0].color.alpha, 0.6) &&
              title0->filters[1].kind == MotionFilterKind::Blur &&
              Near(title0->filters[1].sigma, 0) &&
              title0->filters[2].kind == MotionFilterKind::Glow,
          "C17 keeps the stack in its order");
    Check(title10 != nullptr && Near(title10->filters[1].sigma, 1) &&
              Near(title10->filters[2].sigma, 5) &&
              Near(title10->filters[2].strength, 1.5),
          "C17 mixes each effect's parameters at frame 10");
    // Box 160 x 76.8 at (80, 51.6); overhang 2 + 0.15 x 64; reach
    // 6 + 12 (shadow) + 6 (blur) + 30 (glow).
    Check(title20 != nullptr && title20->has_filter_domain &&
              Near(title20->filter_domain.x, 14.4, 1e-3) &&
              Near(title20->filter_domain.y, -14, 1e-3) &&
              Near(title20->filter_domain.width, 291.2, 1e-3) &&
              Near(title20->filter_domain.height, 208, 1e-3),
          "C17 bounds the pixels its effects may write: " +
              DumpMotionFrame(at20));
    MotionComposition parsed;
    MotionError error;
    Check(
        !ParseMotionComposition(
            R"J({"width":10,"height":10,"fps":25,"duration":5,"root":{"type":"fill","style":{"filter":"sepia(1)"}}})J",
            parsed, error) &&
            error.code == "InvalidValue",
        "C17 refuses an effect it does not know");
    Check(
        !ParseMotionComposition(
            R"J({"width":10,"height":10,"fps":25,"duration":5,"root":{"type":"fill","animate":{"filter":{"interpolate":{"frames":[0,4],"values":["blur(1px)","glow(2px #fff)"]}}}}})J",
            parsed, error) &&
            error.code == "InvalidAnimation",
        "C17 refuses to mix two different stacks");
}

// REM3 -- C18: a picture revealed by a circle, then by a polygon.
void TestC18CircleThenPolygon() {
    const MotionComposition composition = Load("c18-circle-polygon-mask.json");
    MotionFrame at0;
    MotionFrame at10;
    MotionFrame at20;
    MotionFrame at35;
    MotionFrame at45;
    if (!Evaluate(composition, 0, at0) || !Evaluate(composition, 10, at10) ||
        !Evaluate(composition, 20, at20) || !Evaluate(composition, 35, at35) ||
        !Evaluate(composition, 45, at45)) {
        return;
    }
    const MotionEvaluatedNode* iris0 = FindId(at0.root, "iris");
    const MotionEvaluatedNode* iris10 = FindId(at10.root, "iris");
    const MotionEvaluatedNode* iris20 = FindId(at20.root, "iris");
    const double full = 0.75 * std::hypot(400.0, 200.0) / std::sqrt(2.0);
    Check(iris0 != nullptr && iris0->has_clip &&
              iris0->clip_shape.kind == MotionClipKind::Circle &&
              Near(iris0->clip_shape.cx, 400) &&
              Near(iris0->clip_shape.cy, 225) &&
              Near(iris0->clip_shape.rx, 0) && iris20 != nullptr &&
              Near(iris20->clip_shape.rx, full) && iris10 != nullptr &&
              iris10->clip_shape.rx > full / 2 && iris10->clip_shape.rx < full,
          "C18 opens a circle from the centre of the picture, eased out: " +
              DumpMotionFrame(at10));
    const MotionEvaluatedNode* wedge35 = FindId(at35.root, "wedge");
    const MotionEvaluatedNode* wedge45 = FindId(at45.root, "wedge");
    Check(wedge35 != nullptr && wedge35->clip.size() == 4 &&
              Near(wedge35->clip[1].first, 400) &&
              Near(wedge35->clip[2].first, 400) &&
              Near(wedge35->clip[2].second, 325) && wedge45 != nullptr &&
              Near(wedge45->clip[1].first, 600) &&
              FindId(at35.root, "iris") == nullptr,
          "C18 then wipes the picture with a polygon, in its own sequence");
    MotionComposition parsed;
    MotionError error;
    Check(
        !ParseMotionComposition(
            R"J({"width":10,"height":10,"fps":25,"duration":5,"root":{"type":"fill","animate":{"clipPath":{"interpolate":{"frames":[0,4],"values":["circle(0% at 50% 50%)","polygon(0 0, 1px 0, 0 1px)"]}}}}})J",
            parsed, error) &&
            error.code == "InvalidAnimation",
        "C18 refuses to morph a circle into a polygon");
}

// REM3 -- the effects themselves, on known pixels.
void TestEffects() {
    MotionLayer dot;
    dot.width = 64;
    dot.height = 64;
    dot.rgba.assign(64 * 64 * 4, 0.0f);
    const auto at = [](int x, int y) {
        return (static_cast<size_t>(y) * 64 + static_cast<size_t>(x)) * 4;
    };
    for (int channel = 0; channel < 4; ++channel) {
        dot.rgba[at(32, 32) + channel] = 1.0f;
    }
    for (const double sigma : {1.0, 4.0, 5.0}) {
        MotionLayer blurred = dot;
        BlurMotionLayer(blurred, sigma);
        double total = 0.0;
        for (size_t index = 3; index < blurred.rgba.size(); index += 4) {
            total += blurred.rgba[index];
        }
        Check(std::fabs(total - 1.0) < 1e-4 &&
                  std::fabs(blurred.rgba[at(29, 32) + 3] -
                            blurred.rgba[at(35, 32) + 3]) < 1e-6 &&
                  std::fabs(blurred.rgba[at(32, 29) + 3] -
                            blurred.rgba[at(32, 35) + 3]) < 1e-6 &&
                  blurred.rgba[at(32, 32) + 3] > blurred.rgba[at(34, 32) + 3],
              "a blur of deviation " + std::to_string(sigma) +
                  " keeps the dot's coverage and spreads it evenly");
    }
    MotionLayer square;
    square.width = 64;
    square.height = 64;
    square.rgba.assign(64 * 64 * 4, 0.0f);
    for (int y = 24; y < 40; ++y) {
        for (int x = 8; x < 24; ++x) {
            for (int channel = 0; channel < 4; ++channel) {
                square.rgba[at(x, y) + channel] = 1.0f;
            }
        }
    }
    MotionFilter glow;
    glow.kind = MotionFilterKind::Glow;
    glow.sigma = 3.0;
    glow.color = {1.0, 0.8, 0.0, 1.0};
    glow.strength = 16.0;
    MotionLayer glowing = square;
    ApplyMotionFilters(glowing, {glow}, MotionAffine{});
    float highest = 0.0f;
    for (size_t index = 3; index < glowing.rgba.size(); index += 4) {
        highest = std::max(highest, glowing.rgba[index]);
    }
    Check(highest <= 1.0f + 1e-6f && glowing.rgba[at(25, 32) + 3] > 0.9f &&
              std::fabs(glowing.rgba[at(25, 32) + 1] -
                        0.8f * glowing.rgba[at(25, 32) + 3]) < 1e-5f &&
              glowing.rgba[at(60, 32) + 3] == 0.0f,
          "a strong glow saturates at full coverage in its colour, and ends "
          "within its reach");
    MotionFilter shadow;
    shadow.kind = MotionFilterKind::DropShadow;
    shadow.offset_x = 20.0;
    shadow.color = {0.0, 0.0, 0.0, 0.5};
    MotionLayer shadowed = square;
    MotionAffine doubled;
    doubled.a = 2.0;
    doubled.d = 2.0;
    ApplyMotionFilters(shadowed, {shadow}, MotionAffine{});
    Check(std::fabs(shadowed.rgba[at(30, 32) + 3] - 0.5f) < 1e-6f &&
              shadowed.rgba[at(30, 32)] == 0.0f &&
              shadowed.rgba[at(16, 32) + 3] == 1.0f &&
              shadowed.rgba[at(45, 32) + 3] == 0.0f,
          "a sharp shadow is the shape, moved, under it");
    MotionLayer scaled = square;
    ApplyMotionFilters(scaled, {shadow}, doubled);
    Check(std::fabs(scaled.rgba[at(50, 32) + 3] - 0.5f) < 1e-6f,
          "a shadow's offset scales with the node");
    MotionFilter still;
    still.kind = MotionFilterKind::Displace;
    still.amplitude = 0.0;
    still.wavelength = 8.0;
    MotionLayer unmoved = square;
    ApplyMotionFilters(unmoved, {still}, MotionAffine{});
    MotionFilter moving = still;
    moving.amplitude = 4.0;
    moving.seed = 7;
    MotionLayer first = square;
    MotionLayer second = square;
    ApplyMotionFilters(first, {moving}, MotionAffine{});
    ApplyMotionFilters(second, {moving}, MotionAffine{});
    bool bounded = true;
    for (int step = 0; step < 1000; ++step) {
        const double value =
            MotionNoise(step * 0.37, step * 0.11, 0.5, 7, step % 2);
        bounded = bounded && value >= -1.0 && value <= 1.0;
    }
    Check(unmoved.rgba == square.rgba && first.rgba == second.rgba &&
              first.rgba != square.rgba && bounded &&
              MotionNoise(1.25, 2.5, 0.0, 7, 0) ==
                  MotionNoise(1.25, 2.5, 0.0, 7, 0) &&
              MotionNoise(1.25, 2.5, 0.0, 7, 0) !=
                  MotionNoise(1.25, 2.5, 0.0, 8, 0),
          "a displacement is seeded: none without amplitude, the same "
          "pixels twice, other pixels with another seed");
}

// FC9 -- docs/roadmap/FILMCRAFT-2026-10.md. The effects above on a whole
// picture, compared with references a person has looked at
// (tests/fixtures/golden/, tests/golden_image.h): a regression anywhere in
// the frame shows, and says where, instead of on the few pixels a check
// names. CUTMACHINE_BLESS=1 rewrites them.
//
// THE TOLERANCES. These loops are plain float and double arithmetic plus
// std::exp (the Gaussian kernel) and std::floor (the noise lattice), so
// what changes from one machine to the other is the last bits: the
// compiler contracting a multiply and an add into one FMA where the target
// has it (arm64 always; x86-64 only when built for it), and libm's exp.
// Measured on 17 October 2026 (Linux x86-64, clang 18), against the
// references the default -O3 build wrote: rebuilt with FMA everywhere
// (-march=x86-64-v3 -ffp-contract=fast) or with -ffast-math, the shadow
// and glow moves by at most 1.2e-7 and the other two not at all; no code
// value changes (docs/reference/tests-golden.md has the figures). Such a
// difference can only show where a float sits on a rounding edge: one
// code, on a few pixels. The tolerance lets one code through on any sample
// and keeps the 99th percentile at zero; a real change -- a shadow one
// pixel off (max 58), a glow 2.5 % stronger (max 6, p99 4), another seed,
// a wider blur -- fails it.
const char* const kGoldenReferenceDir = GOLDEN_REFERENCE_DIR;
const char* const kGoldenOutputDir = GOLDEN_OUTPUT_DIR;

// A white square, an orange disc with a soft edge and a translucent blue
// band across both, premultiplied: hard edges, a gradient of coverage and
// three colours for the effects to move about.
MotionLayer GoldenScene() {
    MotionLayer layer;
    layer.width = 64;
    layer.height = 64;
    layer.rgba.assign(64 * 64 * 4, 0.0f);
    const auto over = [&](int x, int y, float r, float g, float b, float a) {
        float* pixel = &layer.rgba[(static_cast<size_t>(y) * 64 + x) * 4];
        pixel[0] = r * a + pixel[0] * (1.0f - a);
        pixel[1] = g * a + pixel[1] * (1.0f - a);
        pixel[2] = b * a + pixel[2] * (1.0f - a);
        pixel[3] = a + pixel[3] * (1.0f - a);
    };
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            if (x >= 8 && x < 26 && y >= 30 && y < 50) {
                over(x, y, 1.0f, 1.0f, 1.0f, 1.0f);
            }
            // Coverage falls from 1 to 0 over the disc's last two pixels.
            const float distance =
                std::hypot(x + 0.5f - 42.0f, y + 0.5f - 22.0f);
            const float disc =
                std::clamp((12.0f - distance) / 2.0f, 0.0f, 1.0f);
            if (disc > 0.0f) over(x, y, 1.0f, 0.55f, 0.1f, disc);
            if (x + y >= 54 && x + y < 66) over(x, y, 0.2f, 0.4f, 1.0f, 0.5f);
        }
    }
    return layer;
}

void CheckEffectGolden(const std::string& name,
                       const std::vector<MotionFilter>& filters,
                       GoldenDepth depth) {
    MotionLayer layer = GoldenScene();
    ApplyMotionFilters(layer, filters, MotionAffine{});
    GoldenCheck check;
    check.name = name;
    check.reference_dir = kGoldenReferenceDir;
    check.output_dir = kGoldenOutputDir;
    check.depth = depth;
    // One code on any sample; nothing on 99 % of the pixels. Why: above.
    check.tolerance.max_abs = 1.0;
    check.tolerance.p99 = 0.0;
    check.tolerance.min_psnr = depth == GoldenDepth::Sixteen ? 90.0 : 60.0;
    const GoldenResult result = CheckGoldenImage(
        GoldenImageFromFloat(layer.width, layer.height, 4, layer.rgba), check);
    Check(result.passed, "golden " + result.message);
    if (result.passed) std::cout << "golden " << result.message << '\n';
}

void TestEffectGoldens() {
    MotionFilter blur;
    blur.kind = MotionFilterKind::Blur;
    blur.sigma = 4.0;  // SVG's three box blurs, not the direct kernel
    CheckEffectGolden("motion_effects_blur", {blur}, GoldenDepth::Sixteen);

    MotionFilter shadow;
    shadow.kind = MotionFilterKind::DropShadow;
    shadow.offset_x = 5.0;
    shadow.offset_y = 3.0;
    shadow.sigma = 1.5;  // the direct Gaussian kernel
    shadow.color = {0.1, 0.0, 0.3, 0.8};
    MotionFilter glow;
    glow.kind = MotionFilterKind::Glow;
    glow.sigma = 2.5;
    glow.color = {1.0, 0.9, 0.2, 1.0};
    glow.strength = 2.0;
    CheckEffectGolden("motion_effects_shadow_glow", {shadow, glow},
                      GoldenDepth::Eight);

    MotionFilter displace;
    displace.kind = MotionFilterKind::Displace;
    displace.amplitude = 4.0;
    displace.wavelength = 12.0;
    displace.seed = 7;
    displace.phase = 0.25;
    CheckEffectGolden("motion_effects_displace", {displace},
                      GoldenDepth::Eight);

    // MA7 -- docs/roadmap/MOTION-AFTER-2026-09.md. The three effects of
    // MOTION-DESIGN-2026-09 that had only a few pixels checked, painted on
    // the same scene.
    MotionFilter grain;
    grain.kind = MotionFilterKind::Grain;
    grain.amplitude = 0.3;
    grain.wavelength = 2.0;
    grain.seed = 5;
    grain.phase = 3.0;
    CheckEffectGolden("motion_effects_grain", {grain}, GoldenDepth::Eight);

    MotionFilter goo;
    goo.kind = MotionFilterKind::Goo;
    goo.sigma = 3.0;
    CheckEffectGolden("motion_effects_goo", {goo}, GoldenDepth::Eight);

    MotionFilter slice;
    slice.kind = MotionFilterKind::Slice;
    slice.amplitude = 6.0;
    slice.wavelength = 8.0;
    slice.seed = 7;
    slice.phase = 2.0;
    CheckEffectGolden("motion_effects_slice", {slice}, GoldenDepth::Eight);
}

// MA7 -- the phase of grain(), slice() and displace() is any finite number,
// and an animation that extends past its keys can carry it anywhere. Its
// whole part became the noise's third coordinate by a cast from double to
// int64, undefined past 2^63 (UBSan: float-cast-overflow). Far phases are
// held at 2^53, where a double has no fraction left: the same pixels every
// time, and the same as at 2^53.
void TestFarPhases() {
    for (const MotionFilterKind kind :
         {MotionFilterKind::Grain, MotionFilterKind::Slice,
          MotionFilterKind::Displace}) {
        MotionFilter filter;
        filter.kind = kind;
        filter.amplitude = kind == MotionFilterKind::Grain ? 0.3 : 6.0;
        filter.wavelength = 8.0;
        filter.seed = 7;
        const auto painted = [&](double phase) {
            MotionLayer layer = GoldenScene();
            filter.phase = phase;
            ApplyMotionFilters(layer, {filter}, MotionAffine{});
            return layer.rgba;
        };
        const std::vector<float> limit = painted(9007199254740992.0);
        Check(painted(1e300) == limit && painted(1e300) == painted(1e300) &&
                  painted(-1e300) == painted(-9007199254740992.0) &&
                  limit != GoldenScene().rgba,
              "an effect at a far phase paints as at 2^53, every time (kind " +
                  std::to_string(static_cast<int>(kind)) + ")");
    }
}

// REMOTION-PARITY-2026-09 REM4 -- C19: a card tilted in perspective,
// rotateX fixed, rotateY animated. Its corners are worked out here from
// the rotations and the perspective alone, and compared to the engine's.
void ExpectedQuad(double rotateX, double rotateY, double perspective,
                  double out[4][2]) {
    const double a = rotateX * 3.14159265358979323846 / 180.0;
    const double b = rotateY * 3.14159265358979323846 / 180.0;
    const double corners[4][2] = {
        {-160, -90}, {160, -90}, {160, 90}, {-160, 90}};
    for (int corner = 0; corner < 4; ++corner) {
        const double px = corners[corner][0];
        const double py = corners[corner][1];
        // rotateY, then rotateX, about the card's centre (320, 180).
        const double x = px * std::cos(b);
        const double y = py * std::cos(a) + px * std::sin(b) * std::sin(a);
        const double z = py * std::sin(a) - px * std::sin(b) * std::cos(a);
        const double w = perspective > 0.0 ? 1.0 - z / perspective : 1.0;
        out[corner][0] = 320.0 + x / w;
        out[corner][1] = 180.0 + y / w;
    }
}

void TestC19Perspective() {
    const MotionComposition composition = Load("c19-perspective-card.json");
    for (const int64_t frame : {0, 10, 20}) {
        MotionFrame evaluated;
        if (!Evaluate(composition, frame, evaluated)) {
            continue;
        }
        const MotionEvaluatedNode* card = FindId(evaluated.root, "card");
        const double turn = card != nullptr ? card->rotate_y : std::nan("");
        double expected[4][2];
        ExpectedQuad(10.0, turn, 800.0, expected);
        bool same = card != nullptr && card->has_3d && card->has_quad &&
                    Near(evaluated.root.perspective, 800);
        for (int corner = 0; same && corner < 4; ++corner) {
            same = Near(card->quad[corner].first, expected[corner][0], 1e-6) &&
                   Near(card->quad[corner].second, expected[corner][1], 1e-6);
        }
        Check(same, "C19 frame " + std::to_string(frame) +
                        " places the card's corners where the rotations and "
                        "the perspective put them: " +
                        DumpMotionFrame(evaluated).substr(0, 900));
        if (frame == 20 && card != nullptr) {
            // Turned away on its right: the right edge is shorter.
            const double leftEdge = card->quad[3].second - card->quad[0].second;
            const double rightEdge =
                card->quad[2].second - card->quad[1].second;
            // The depth row is the perspective times the rotated z axis:
            // -(Rx Ry)[2] / d, with (Rx Ry)[2] = (-cos a sin b, sin a,
            // cos a cos b).
            const double a = 10.0 * 3.14159265358979323846 / 180.0;
            const double b = 35.0 * 3.14159265358979323846 / 180.0;
            Check(Near(card->rotate_y, 35) && rightEdge < leftEdge &&
                      Near(card->matrix3d[12],
                           std::cos(a) * std::sin(b) / 800.0, 1e-12) &&
                      Near(card->matrix3d[13], -std::sin(a) / 800.0, 1e-12) &&
                      Near(card->matrix3d[14],
                           -std::cos(a) * std::cos(b) / 800.0, 1e-12),
                  "C19 at frame 20 turns 35 degrees, its far edge shorter, "
                  "the perspective carried by the matrix's depth row");
        }
    }
    // Without a perspective, the card is only foreshortened: the same
    // corners, w = 1.
    MotionComposition flat = composition;
    flat.root.style.erase(
        std::remove_if(
            flat.root.style.begin(), flat.root.style.end(),
            [](const auto& entry) { return entry.first == "perspective"; }),
        flat.root.style.end());
    MotionFrame orthographic;
    if (Evaluate(flat, 20, orthographic)) {
        const MotionEvaluatedNode* card = FindId(orthographic.root, "card");
        double expected[4][2];
        ExpectedQuad(10.0, 35.0, 0.0, expected);
        Check(card != nullptr && card->has_quad &&
                  Near(card->quad[1].first, expected[1][0], 1e-6) &&
                  Near(card->quad[2].second, expected[2][1], 1e-6) &&
                  Near(card->quad[2].second - card->quad[1].second,
                       card->quad[3].second - card->quad[0].second, 1e-6),
              "C19 without perspective foreshortens without depth");
    }
    MotionComposition parsed;
    MotionError error;
    Check(
        !ParseMotionComposition(
            R"J({"width":10,"height":10,"fps":25,"duration":5,"root":{"type":"fill","style":{"perspective":-5}}})J",
            parsed, error) &&
            error.code == "InvalidValue" &&
            ParseMotionComposition(
                R"J({"width":10,"height":10,"fps":25,"duration":5,"root":{"type":"fill","style":{"perspective":"none"}}})J",
                parsed, error),
        "C19 refuses a negative perspective and takes none");
}

void TestCorpusDeterminism() {
    CheckDeterminism("c01-centered-card.json", {0, 12, 49});
    CheckDeterminism("c02-staggered-credit.json", {0, 3, 4, 9, 12, 30, 74});
    CheckDeterminism("c03-word-cascade.json", {0, 4, 39});
    CheckDeterminism("c04-series-timescale.json", {0, 9, 10, 18, 29});
    CheckDeterminism("c05-repeat-palette.json", {0, 2, 29});
    CheckDeterminism("c06-path-trim.json", {0, 10, 20, 24});
    CheckDeterminism("c07-polygon-clip.json", {0, 5, 10, 19});
    CheckDeterminism("c08-logo-fit.json", {0, 12, 24});
    CheckDeterminism("c09-font-control.json", {0, 12, 24});
    CheckDeterminism("c10-variants-formats.json", {0, 12, 49});
    CheckDeterminism("c12-rounded-video.json", {0, 1, 25, 39, 40});
    CheckDeterminism("c13-detached-sound.json", {0, 10, 30, 59, 74});
    CheckDeterminism("c15-fade-slide.json", {0, 19, 25, 30, 45, 69});
    CheckDeterminism("c16-animated-gradient.json", {0, 10, 20});
    CheckDeterminism("c17-shadow-blur-glow.json", {0, 10, 20});
    CheckDeterminism("c18-circle-polygon-mask.json", {0, 10, 20, 35, 45});
    CheckDeterminism("c19-perspective-card.json", {0, 10, 20, 29});
}

}  // namespace

int main() {
    TestC01CenteredCard();
    TestC02StaggeredCredit();
    TestC03WordCascade();
    TestC04SeriesAndTimeScale();
    TestC05RepeatPalette();
    TestC06PathTrim();
    TestC07PolygonClip();
    TestC08LogoFitAndAlpha();
    TestC09FontControl();
    TestC10VariantsAndFormats();
    TestC12RoundedVideo();
    TestC13DetachedSound();
    TestC14Precomposition();
    TestC15FadeThenSlide();
    TestC16AnimatedGradient();
    TestC17EffectStack();
    TestC18CircleThenPolygon();
    TestEffects();
    TestEffectGoldens();
    TestFarPhases();
    TestC19Perspective();
    TestCorpusDeterminism();
    std::cout << "Motion parity: " << failures << " failure(s)\n";
    return failures == 0 ? 0 : 1;
}
