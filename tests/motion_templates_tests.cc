// M5 -- docs/roadmap/MOTION-2026-09.md. The templates shipped with the
// binary, the placement an agent asks for by anchor, and the safe-area check
// of an evaluated frame. Portable: no AppKit, no FFmpeg; text is measured by
// a fake whose widths are known, so every bound below is exact.
//
// MB1 -- docs/roadmap/MOTION-BIBLIOTHEQUE-2026-10.md. And the motion
// library: the piece file, the directory read beside the shipped catalog,
// and the way a composition of a project goes back to the shelf.

#include "Document.h"
#include "Json.h"
#include "MotionComposition.h"
#include "MotionDocument.h"
#include "MotionEvaluate.h"
#include "MotionLibraryBase.h"
#include "MotionPlacement.h"
#include "MotionSafeArea.h"
#include "MotionTemplates.h"

#include <sqlite3.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "TestSupport.h"

namespace {

using mcp_json::Value;

// Every byte advances half the font size; lines break only on '\n'.
class FakeMeasurer final : public MotionMeasurer {
public:
    bool LayoutText(const std::string& text, const MotionTextStyle& style,
                    double max_width, MotionTextLayout& layout,
                    MotionError&) override {
        layout = MotionTextLayout{};
        const double lineHeight = style.font_size * 1.2;
        MotionTextLine line;
        line.text = text;
        const double advance = style.font_size * 0.5;
        for (size_t index = 0; index < text.size(); ++index) {
            line.clusters.push_back(
                {index, 1, advance * static_cast<double>(index), advance});
        }
        line.width = advance * static_cast<double>(text.size());
        line.height = lineHeight;
        line.baseline = style.font_size * 0.8;
        if (max_width >= 0.0 && style.text_align == "center") {
            line.x = (max_width - line.width) / 2.0;
        }
        layout.lines.push_back(line);
        layout.width = line.width;
        layout.height = lineHeight;
        return true;
    }

    bool ImageSize(const std::string&, double& width, double& height,
                   MotionError&) override {
        width = 400.0;
        height = 160.0;
        return true;
    }
};

// M5b -- like FakeMeasurer, but a line breaks at a space when the next word
// would pass max_width, as Core Text does: what a measure (`maxWidth`)
// does to a phrase too long for it.
class WrappingMeasurer final : public MotionMeasurer {
public:
    bool LayoutText(const std::string& text, const MotionTextStyle& style,
                    double max_width, MotionTextLayout& layout,
                    MotionError&) override {
        layout = MotionTextLayout{};
        const double advance = style.font_size * 0.5;
        const double lineHeight =
            style.line_height > 0.0 ? style.line_height : style.font_size * 1.2;
        std::vector<std::string> lines;
        std::string current;
        size_t at = 0;
        while (at <= text.size()) {
            const size_t space = std::min(text.find(' ', at), text.size());
            const std::string word = text.substr(at, space - at);
            std::string longer = current;
            if (!longer.empty()) {
                longer += ' ';
            }
            longer += word;
            if (!current.empty() && max_width >= 0.0 &&
                advance * static_cast<double>(longer.size()) >
                    max_width + 1e-9) {
                lines.push_back(current);
                current = word;
            } else {
                current = longer;
            }
            at = space + 1;
        }
        lines.push_back(current);
        for (size_t index = 0; index < lines.size(); ++index) {
            MotionTextLine line;
            line.text = lines[index];
            line.width = advance * static_cast<double>(line.text.size());
            line.height = lineHeight;
            line.y = lineHeight * static_cast<double>(index);
            line.baseline = line.y + style.font_size * 0.8;
            if (max_width >= 0.0 && style.text_align == "center") {
                line.x = (max_width - line.width) / 2.0;
            }
            for (size_t byte = 0; byte < line.text.size(); ++byte) {
                line.clusters.push_back(
                    {byte, 1, advance * static_cast<double>(byte), advance});
            }
            layout.width = std::max(layout.width, line.width);
            layout.lines.push_back(line);
        }
        layout.height = lineHeight * static_cast<double>(lines.size());
        return true;
    }

    bool ImageSize(const std::string&, double& width, double& height,
                   MotionError&) override {
        width = 400.0;
        height = 160.0;
        return true;
    }
};

const MotionTemplateInfo* Find(const std::vector<MotionTemplateInfo>& list,
                               const std::string& name) {
    for (const MotionTemplateInfo& info : list) {
        if (info.name == name) return &info;
    }
    return nullptr;
}

// What a composition resolves to in `format` (its own size and defaults for
// an empty name): the size, then every prop with the value it takes there.
std::string ResolvedIn(const Value& definition, const std::string& format) {
    MotionComposition composition;
    MotionComposition resolved;
    MotionError error;
    Value props;
    if (!ParseMotionComposition(definition, composition, error) ||
        !ResolveMotionVariant(composition, "", format, Value(), resolved, props,
                              error)) {
        return "unresolved: " + error.message;
    }
    std::string text =
        std::to_string(resolved.width) + "x" + std::to_string(resolved.height);
    for (const MotionPropDefinition& prop : composition.props) {
        const Value* set = props.Find(prop.name);
        text += " " + prop.name + "=" +
                (set != nullptr ? set->Dump() : prop.default_value.Dump());
    }
    return text;
}

// Every template, in every format it declares and for every value of its
// enum props, evaluates at its first, middle and last frames: the parser
// alone accepted a logo whose corner tokens gave an inset of "auto", which
// only evaluation refuses.
void CheckCatalog() {
    std::vector<MotionTemplateInfo> templates;
    std::string error;
    Check(MotionTemplateCatalog(templates, error), "catalog loads: " + error);
    // MB1 -- every piece file of assets/motion-templates ships: the three
    // of M5 are named here, and whatever joins them is held to the same
    // checks below.
    Check(templates.size() >= 3 && Find(templates, "nom") != nullptr &&
              Find(templates, "carton") != nullptr &&
              Find(templates, "logo") != nullptr,
          "the catalog holds nom, carton and logo");
    FakeMeasurer measurer;
    for (const MotionTemplateInfo& info : templates) {
        Check(!info.description.empty() && info.path.empty(),
              info.name + " is described, and is no file of a library");
        const Value* formats = info.definition.Find("formats");
        Check(formats != nullptr && formats->Find("9x16") != nullptr &&
                  formats->Find("16x9") != nullptr &&
                  formats->Find("1x1") != nullptr,
              info.name + " has a 9x16, a 16x9 and a 1x1 layout");
        if (formats == nullptr) continue;
        for (const auto& [format, size] : formats->AsObject()) {
            int64_t width = 0;
            int64_t height = 0;
            size.Find("width")->AsInt64(width);
            size.Find("height")->AsInt64(height);
            Value instance;
            std::string chosen;
            const std::string label = info.name + " " + format;
            if (!InstantiateMotionTemplate(
                    info.definition, static_cast<int32_t>(width),
                    static_cast<int32_t>(height), instance, chosen, error)) {
                Check(false, label + " instantiates: " + error);
                continue;
            }
            Check(chosen == format && instance.Find("formats") == nullptr,
                  label + " is chosen by size and drops the formats");
            // MB1 -- or keeps them, rebased: each format still resolves to
            // what it was in the piece, the instance itself to the chosen
            // one, and the piece saved from it instantiates in every size
            // as the piece it came from does.
            Value whole;
            Value saved;
            std::string keptFormat;
            if (!InstantiateMotionTemplate(info.definition,
                                           static_cast<int32_t>(width),
                                           static_cast<int32_t>(height), whole,
                                           keptFormat, error, true) ||
                !MotionPieceDefinition(whole, saved, error)) {
                Check(false,
                      label + " instantiates with its formats: " + error);
                continue;
            }
            Check(keptFormat == format && whole.Find("formats") != nullptr &&
                      whole.Find("formats")->AsObject().size() ==
                          formats->AsObject().size() &&
                      whole.Find("formats")->Find(format)->Find("props") ==
                          nullptr &&
                      ResolvedIn(whole, "") ==
                          ResolvedIn(info.definition, format),
                  label + " keeps its formats around the one chosen: " +
                      ResolvedIn(whole, "") + " / " +
                      ResolvedIn(info.definition, format));
            for (const auto& [other, otherSize] : formats->AsObject()) {
                Check(ResolvedIn(whole, other) ==
                          ResolvedIn(info.definition, other),
                      label + " still resolves " + other +
                          " as the piece: " + ResolvedIn(whole, other) + " / " +
                          ResolvedIn(info.definition, other));
                int64_t otherWidth = 0;
                int64_t otherHeight = 0;
                otherSize.Find("width")->AsInt64(otherWidth);
                otherSize.Find("height")->AsInt64(otherHeight);
                Value fromPiece;
                Value fromSaved;
                std::string ignored;
                Check(InstantiateMotionTemplate(
                          info.definition, static_cast<int32_t>(otherWidth),
                          static_cast<int32_t>(otherHeight), fromPiece, ignored,
                          error) &&
                          InstantiateMotionTemplate(
                              saved, static_cast<int32_t>(otherWidth),
                              static_cast<int32_t>(otherHeight), fromSaved,
                              ignored, error) &&
                          fromSaved.Dump() == fromPiece.Dump(),
                      label + ", saved back, is the same piece in " + other +
                          ": " + error);
            }
            MotionComposition composition;
            MotionError motion;
            if (!ParseMotionComposition(instance, composition, motion)) {
                Check(false, label + " parses: " + motion.message);
                continue;
            }
            // One set of props per enum value, the defaults otherwise.
            std::vector<Value> propSets = {Value()};
            for (const MotionPropDefinition& prop : composition.props) {
                if (prop.type != MotionPropType::Enum) continue;
                propSets.clear();
                for (const std::string& value : prop.values) {
                    Value props = Value::MakeObject();
                    props.Set(prop.name, Value::MakeString(value));
                    propSets.push_back(std::move(props));
                }
            }
            for (const Value& props : propSets) {
                for (const int64_t frame :
                     {int64_t{0}, composition.duration / 2,
                      composition.duration - 1}) {
                    MotionFrame evaluated;
                    Check(EvaluateMotionFrame(composition, props, frame,
                                              measurer, evaluated, motion) &&
                              evaluated.width == width &&
                              evaluated.height == height,
                          label + " " + props.Dump() + " evaluates at frame " +
                              std::to_string(frame) + ": " + motion.code +
                              " at " + motion.path + ": " + motion.message);
                }
            }
        }
    }

    // A format's values become the defaults of the instance.
    const MotionTemplateInfo* nom = Find(templates, "nom");
    Value instance;
    std::string format;
    Check(nom != nullptr &&
              InstantiateMotionTemplate(nom->definition, 1920, 1080, instance,
                                        format, error) &&
              format == "16x9" && instance.Find("width")->Dump() == "1920" &&
              instance.Find("props")->Find("corps")->Find("default")->Dump() ==
                  "56",
          "nom in 16x9 takes the format's size and body: " + instance.Dump());
    // A size it has no layout for is refused, naming those it has.
    Check(nom != nullptr &&
              !InstantiateMotionTemplate(nom->definition, 1280, 720, instance,
                                         format, error) &&
              error.find("1280x720") != std::string::npos &&
              error.find("16x9 1920x1080") != std::string::npos,
          "a 1280x720 timeline is refused with the known formats: " + error);
}

// A 25 fps timeline: V1 holds A [0, 150) and B [150, 300) of one camera
// source -- a jump cut -- then C [300, 400) of another; A1 the sound of A;
// MB3 -- the piece is usable from every surface's catalog. Changing the
// selected words moves the band; progression uses their measured length.
void CheckHighlighterPiece() {
    std::vector<MotionTemplateInfo> pieces;
    std::string error;
    if (!MotionTemplateCatalog(pieces, error)) {
        Check(false, "the catalog loads for the highlighter: " + error);
        return;
    }
    const auto* piece = Find(pieces, "surligneur");
    Check(piece != nullptr, "the highlighter is on the shipped shelf");
    if (piece == nullptr) return;
    Value instance;
    std::string format;
    MotionComposition composition;
    MotionError motion;
    if (!InstantiateMotionTemplate(piece->definition, 1080, 1920, instance,
                                   format, error) ||
        !ParseMotionComposition(instance, composition, motion)) {
        Check(false, "the highlighter instantiates: " + error + motion.message);
        return;
    }
    Value props = Value::MakeObject();
    props.Set("texte", Value::MakeString("un deux trois quatre"));
    props.Set("premier", Value::MakeInt(1));
    props.Set("nombre", Value::MakeInt(2));
    FakeMeasurer measurer;
    const int64_t frames[] = {8, 20, 32};
    const double painted[] = {0.0, 248.0, 496.0};
    for (size_t i = 0; i < 3; ++i) {
        MotionFrame frame;
        if (!EvaluateMotionFrame(composition, props, frames[i], measurer, frame,
                                 motion)) {
            Check(false, "the highlighter evaluates: " + motion.message);
            continue;
        }
        const auto& text = frame.root.children.front();
        Check(
            text.highlights.size() == 1 &&
                std::abs(text.highlights.front().painted - painted[i]) < 1e-3 &&
                std::abs(text.highlights.front().box.width - 496.0) < 1e-3,
            "the shelf's band advances over words and their space");
    }
    props.Set("premier", Value::MakeInt(3));
    props.Set("nombre", Value::MakeInt(1));
    MotionFrame changed;
    Check(EvaluateMotionFrame(composition, props, 32, measurer, changed,
                              motion) &&
              changed.root.children.front().highlights.size() == 1 &&
              std::abs(
                  changed.root.children.front().highlights.front().box.width -
                  304.0) < 1e-3,
          "changing the chosen words recomputes the band's bounds");
}

// V2 a cutaway at [250, 275); V3 empty; A2 the sound of a separate
// recorder, [0, 300).
const Ulid kCamera = "01K40000000000000000000001";
const Ulid kRecorder = "01K40000000000000000000002";
const Ulid kV1 = "01K40000000000000000000003";
const Ulid kA1 = "01K40000000000000000000004";
const Ulid kV2 = "01K40000000000000000000005";
const Ulid kV3 = "01K40000000000000000000006";
const Ulid kA2 = "01K40000000000000000000007";
const Ulid kClipA = "01K40000000000000000000008";
const Ulid kClipB = "01K40000000000000000000009";
const Ulid kSoundA = "01K4000000000000000000000A";
const Ulid kCutaway = "01K4000000000000000000000B";
const Ulid kRecorded = "01K4000000000000000000000C";
const Ulid kTitle = "01K4000000000000000000000D";
const Ulid kTitle30 = "01K4000000000000000000000E";
const Ulid kOther = "01K4000000000000000000000F";
const Ulid kClipC = "01K4000000000000000000000G";

DocumentClip Clip(const Ulid& id, const Ulid& source, int64_t timelineIn,
                  int64_t sourceIn, int64_t frames) {
    DocumentClip clip;
    clip.id = id;
    clip.source_id = source;
    clip.timeline_in = {timelineIn, 25};
    clip.source_in = {sourceIn, 25};
    clip.duration = {frames, 25};
    return clip;
}

Document PlacementFixture() {
    Document document;
    document.sequence.width = 1080;
    document.sequence.height = 1920;
    document.sources = {{kCamera, "A.MP4", {25, 1}, {1000, 25}},
                        {kRecorder, "A.WAV", {25, 1}, {1000, 25}},
                        {kOther, "B.MP4", {25, 1}, {1000, 25}}};
    DocumentTrack v1{kV1, "video", 0, {}};
    v1.clips = {Clip(kClipA, kCamera, 0, 0, 150),
                Clip(kClipB, kCamera, 150, 150, 150),
                Clip(kClipC, kOther, 300, 0, 100)};
    DocumentTrack a1{kA1, "audio", 1, {}};
    a1.clips = {Clip(kSoundA, kCamera, 0, 0, 150)};
    DocumentTrack v2{kV2, "video", 2, {}};
    v2.clips = {Clip(kCutaway, kCamera, 250, 500, 25)};
    DocumentTrack v3{kV3, "video", 3, {}};
    DocumentTrack a2{kA2, "audio", 4, {}};
    a2.clips = {Clip(kRecorded, kRecorder, 0, 0, 300)};
    document.sequence.tracks = {v1, a1, v2, v3, a2};
    DocumentComposition title;
    title.id = kTitle;
    title.name = "Titre";
    std::string error;
    Value::Parse(R"({"width":1080,"height":1920,"fps":25,"duration":100,)"
                 R"("root":{"type":"fill"}})",
                 title.definition, error);
    DocumentComposition title30 = title;
    title30.id = kTitle30;
    Value::Parse(R"({"width":1080,"height":1920,"fps":30,"duration":100,)"
                 R"("root":{"type":"fill"}})",
                 title30.definition, error);
    document.compositions = {title, title30};
    return document;
}

bool Place(const Document& document, const MotionPlacementRequest& request,
           MotionPlacement& placement, std::string& errorName) {
    std::string error;
    const bool placed =
        ResolveMotionPlacement(document, request, placement, errorName, error);
    if (!placed && errorName.empty()) errorName = "?" + error;
    return placed;
}

bool Is(const MotionPlacement& placement, const Ulid& track, int64_t in,
        int64_t frames) {
    return placement.track_id == track &&
           placement.timeline_in == RationalTime{in, 25} &&
           placement.duration == RationalTime{frames, 25};
}

std::string Text(const RationalTime& time) {
    return std::to_string(time.value) + "/" + std::to_string(time.rate);
}

std::string Describe(const MotionPlacement& placement) {
    return placement.track_id + " " + Text(placement.timeline_in) + " " +
           Text(placement.duration);
}

void CheckPlacement() {
    const Document document = PlacementFixture();
    MotionPlacement placement;
    std::string errorName;

    // On a clip: the lowest video track above it that is free, for the
    // whole composition when the clip leaves room for it.
    MotionPlacementRequest request;
    request.composition_id = kTitle;
    request.clip_id = kClipA;
    Check(Place(document, request, placement, errorName) &&
              Is(placement, kV2, 0, 100),
          "a name on clip A goes on V2 over [0, 100): " + errorName + " " +
              Describe(placement));
    // B from 10 frames in: [160, 260) meets the cutaway on V2, so V3.
    request.clip_id = kClipB;
    request.offset = {10, 25};
    Check(Place(document, request, placement, errorName) &&
              Is(placement, kV3, 160, 100),
          "the cutaway on V2 sends it to V3: " + errorName + " " +
              Describe(placement));
    // B from 100 frames in: 50 frames left before C, another source.
    request.offset = {100, 25};
    Check(Place(document, request, placement, errorName) &&
              Is(placement, kV3, 250, 50),
          "the shot's end bounds the default duration: " + errorName + " " +
              Describe(placement));
    // A from 120 frames in: the jump cut to B is the same shot.
    request.clip_id = kClipA;
    request.offset = {120, 25};
    Check(Place(document, request, placement, errorName) &&
              Is(placement, kV2, 120, 100),
          "a jump cut does not end the shot: " + errorName + " " +
              Describe(placement));
    request.clip_id = kClipB;
    request.offset = {150, 25};
    Check(!Place(document, request, placement, errorName) &&
              errorName == "InvalidTimelineIn",
          "an offset past the clip is refused: " + errorName);
    request.offset = {0, 25};
    request.duration = RationalTime{101, 25};
    Check(!Place(document, request, placement, errorName) &&
              errorName == "InvalidTimelineIn",
          "a duration longer than the composition is refused: " + errorName);
    request.duration = RationalTime{1, 50};
    Check(!Place(document, request, placement, errorName) &&
              errorName == "InvalidTimelineIn",
          "half a frame is refused: " + errorName);
    request.duration.reset();
    request.track_id = kV2;
    request.clip_id = kClipB;
    request.offset = {10, 25};
    Check(!Place(document, request, placement, errorName) &&
              errorName == "InvalidTimelineIn",
          "a named track taken over the range is refused: " + errorName);
    request.track_id = kA1;
    Check(!Place(document, request, placement, errorName) &&
              errorName == "UnknownTrack",
          "a sound track is refused: " + errorName);
    request.track_id.clear();
    request.clip_id = "01K4000000000000000000000Z";
    Check(!Place(document, request, placement, errorName) &&
              errorName == "UnknownClip",
          "an unknown clip is refused: " + errorName);
    request.clip_id = kClipA;
    request.composition_id = "01K4000000000000000000000Y";
    Check(!Place(document, request, placement, errorName) &&
              errorName == "UnknownComposition",
          "an unknown composition is refused: " + errorName);

    // The film's edges: above the bottom video track.
    request = MotionPlacementRequest{};
    request.composition_id = kTitle;
    request.anchor = MotionAnchor::Start;
    Check(Place(document, request, placement, errorName) &&
              Is(placement, kV2, 0, 100),
          "at the start: " + errorName + " " + Describe(placement));
    request.anchor = MotionAnchor::End;
    Check(Place(document, request, placement, errorName) &&
              Is(placement, kV2, 300, 100),
          "at the end: " + errorName + " " + Describe(placement));
    request.offset = {1, 25};
    Check(!Place(document, request, placement, errorName) &&
              errorName == "InvalidTimelineIn",
          "an offset at the end is refused: " + errorName);

    // A composition at 30 fps lasts 100/30 s: 83 whole frames at 25.
    request = MotionPlacementRequest{};
    request.composition_id = kTitle30;
    request.clip_id = kClipA;
    Check(Place(document, request, placement, errorName) &&
              Is(placement, kV2, 0, 83),
          "a 30 fps composition is floored to whole frames: " + errorName +
              " " + Describe(placement));

    // A passage: its words start and end between frames. 3.37 s falls in
    // frame 84, 5.01 s ends in frame 125, so [84, 126).
    request = MotionPlacementRequest{};
    request.composition_id = kTitle;
    request.anchor = MotionAnchor::Span;
    request.span_source_id = kCamera;
    request.span_in = {337, 100};
    request.span_end = RationalTime{501, 100};
    Check(Place(document, request, placement, errorName) &&
              Is(placement, kV2, 84, 42),
          "a passage covers every frame a word of it is heard in: " +
              errorName + " " + Describe(placement));
    // Unbounded, as long as the composition within the shot that plays its
    // start: here the whole composition, and 50 frames from 10 s.
    request.span_end.reset();
    Check(Place(document, request, placement, errorName) &&
              Is(placement, kV2, 84, 100),
          "without an end, the composition's length: " + errorName + " " +
              Describe(placement));
    request.span_in = {10, 1};
    Check(Place(document, request, placement, errorName) &&
              Is(placement, kV3, 250, 50),
          "without an end, the shot of the speaker bounds it: " + errorName +
              " " + Describe(placement));
    request.span_in = {337, 100};
    // Longer than the composition: refused rather than cut.
    request.span_end = RationalTime{10, 1};
    Check(!Place(document, request, placement, errorName) &&
              errorName == "InvalidTimelineIn",
          "a passage longer than the composition is refused: " + errorName);
    // Heard only on the recorder's track: above the bottom video track,
    // and the recorder's clip bounds it.
    request.span_source_id = kRecorder;
    request.span_end.reset();
    request.span_in = {11, 1};
    Check(Place(document, request, placement, errorName) &&
              Is(placement, kV2, 275, 25),
          "a passage heard on a sound track only: " + errorName + " " +
              Describe(placement));
    request.span_source_id = "01K4000000000000000000000X";
    Check(!Place(document, request, placement, errorName) &&
              errorName == "UnknownClip",
          "a passage no clip plays is refused: " + errorName);
}

// Builders of evaluated frames for the safe-area check: a 1080x1920 frame,
// safe area [54, 1026] x [96, 1824] at 5 %.
MotionEvaluatedNode TextNode(const std::string& id, double x, double y,
                             double width,
                             const std::vector<std::string>& lines) {
    MotionEvaluatedNode node;
    node.id = id;
    node.uid = "uid-" + id;
    node.type = MotionNodeType::Text;
    node.box = {x, y, width, 60.0 * static_cast<double>(lines.size())};
    for (size_t index = 0; index < lines.size(); ++index) {
        MotionTextLine line;
        line.text = lines[index];
        line.width = 20.0 * static_cast<double>(lines[index].size());
        line.height = 60.0;
        line.y = 60.0 * static_cast<double>(index);
        node.text_layout.lines.push_back(line);
        node.text += lines[index];
    }
    return node;
}

MotionFrame Frame(std::vector<MotionEvaluatedNode> children,
                  int64_t number = 0) {
    MotionFrame frame;
    frame.width = 1080;
    frame.height = 1920;
    frame.frame = number;
    frame.root.type = MotionNodeType::Fill;
    frame.root.box = {0, 0, 1080, 1920};
    frame.root.children = std::move(children);
    return frame;
}

MotionSafeAreaReport CheckOne(const MotionFrame& frame) {
    MotionSafeAreaReport report;
    CheckMotionFrameSafeArea(frame, report);
    return report;
}

void CheckSafeArea() {
    // Inside, on the edge (half a pixel of tolerance), and past it.
    MotionSafeAreaReport report =
        CheckOne(Frame({TextNode("a", 54, 96, 400, {"Camille"})}));
    Check(report.issues.empty() && report.checked_frames == 1 &&
              report.safe.x == 54 && report.safe.width == 972,
          "a text on the safe area's corner is inside");
    report = CheckOne(Frame({TextNode("a", 900, 1000, 400, {"Camille"})}));
    Check(report.issues.size() == 1 &&
              report.issues[0].kind == "outside_safe_area" &&
              report.issues[0].edges == std::vector<std::string>{"right"} &&
              report.issues[0].node_id == "a" &&
              report.issues[0].bounds.x == 900 &&
              report.issues[0].bounds.width == 140,
          "140 px of text from x 900 crosses the right edge");

    // Under half opacity it is not counted; a parent's opacity counts.
    MotionEvaluatedNode faint = TextNode("a", 900, 1000, 400, {"Camille"});
    faint.opacity = 0.4;
    Check(CheckOne(Frame({faint})).issues.empty(),
          "a text at 0.4 opacity is not counted");
    MotionEvaluatedNode parent;
    parent.type = MotionNodeType::Box;
    parent.box = {0, 0, 1080, 1920};
    parent.opacity = 0.7;
    MotionEvaluatedNode child = TextNode("a", 900, 1000, 400, {"Camille"});
    child.opacity = 0.7;
    parent.children = {child};
    Check(CheckOne(Frame({parent})).issues.empty(),
          "0.7 under 0.7 is 0.49: not counted");

    // Transforms as the renderer applies them: a translation, and a scale
    // about the box's centre.
    MotionEvaluatedNode moved = TextNode("a", 100, 1000, 400, {"Camille"});
    moved.translate_x = -60;
    report = CheckOne(Frame({moved}));
    Check(report.issues.size() == 1 && report.issues[0].bounds.x == 40 &&
              report.issues[0].edges == std::vector<std::string>{"left"},
          "a translation moves the bounds");
    MotionEvaluatedNode scaled = TextNode("a", 400, 100, 280, {"Camille"});
    scaled.scale_x = 2.0;
    scaled.scale_y = 2.0;
    report = CheckOne(Frame({scaled}));
    // Box [400, 680] x [100, 160], centre (540, 130): doubled, [260, 820]
    // x [70, 190] -- over the top edge.
    Check(report.issues.size() == 1 && report.issues[0].bounds.x == 260 &&
              report.issues[0].bounds.y == 70 &&
              report.issues[0].edges == std::vector<std::string>{"top"},
          "a scale about the centre grows the bounds both ways");

    // Backgrounds: a band across the whole width bleeds on purpose; one that
    // stops between the safe area and the frame's edge does not.
    MotionEvaluatedNode band;
    band.type = MotionNodeType::Box;
    band.has_background = true;
    band.box = {0, 1500, 1080, 200};
    Check(CheckOne(Frame({band})).issues.empty(),
          "a full-width band bleeds on both sides");
    band.box = {86, 1500, 954, 200};
    report = CheckOne(Frame({band}));
    Check(report.issues.size() == 1 && report.issues[0].text.empty() &&
              report.issues[0].edges == std::vector<std::string>{"right"},
          "a band ending at 1040 crosses the right edge");
    band.background.alpha = 0.3;
    Check(CheckOne(Frame({band})).issues.empty(),
          "a background at 0.3 alpha is not counted");

    // A word broken between two letters, against breaks at a space and at
    // a hyphen.
    report = CheckOne(Frame(
        {TextNode("nom", 100, 1000, 700, {"Anticonstitution", "nellement"}),
         TextNode("autre", 100, 1200, 700,
                  {"Marie-Charlotte de ", "Rochefoucauld-", "Montmorency"})}));
    Check(report.issues.size() == 1 && report.issues[0].kind == "word_split" &&
              report.issues[0].text == "Anticonstitution|nellement" &&
              report.issues[0].node_id == "nom",
          "a word broken between letters is reported, a line broken at a "
          "space or a hyphen is not");

    // Over several frames, one issue per element, first and last frames.
    report = MotionSafeAreaReport{};
    for (int64_t frame = 10; frame <= 12; ++frame) {
        CheckMotionFrameSafeArea(
            Frame({TextNode("a", 900, 1000, 400, {"Camille"})}, frame), report);
    }
    Check(report.checked_frames == 3 && report.issues.size() == 1 &&
              report.issues[0].first_frame == 10 &&
              report.issues[0].last_frame == 12,
          "an issue seen on three frames is one issue from 10 to 12");
    const std::string json = SerializeMotionSafeAreaReport(report);
    Check(json.find("\"inside\":false") != std::string::npos &&
              json.find("\"edges\":[\"right\"]") != std::string::npos &&
              json.find("\"safe_area\":{\"x\":54,\"y\":96,\"width\":972,"
                        "\"height\":1728}") != std::string::npos,
          "the report as the agent reads it: " + json);

    // A wider margin for a platform's own interface.
    report = MotionSafeAreaReport{};
    report.zone = DefaultMotionSafeZone(10);
    CheckMotionFrameSafeArea(Frame({TextNode("a", 54, 96, 400, {"Camille"})}),
                             report);
    Check(report.issues.size() == 1 &&
              report.issues[0].edges == std::vector<std::string>{"left", "top"},
          "at 10 % the corner of the 5 % area is outside");
    Check(SerializeMotionSafeAreaReport(report).find(
              "\"zone\":\"default\",\"margins\":{\"top\":10,\"bottom\":10,"
              "\"left\":10,\"right\":10},\"margin_percent\":10,") !=
              std::string::npos,
          "the default zone is named, with its margin as before: " +
              SerializeMotionSafeAreaReport(report));
}

// PRD38 -- the zone of a vertical social video. The 1080x1920 frame of the
// builders above, `children` included, scaled by `scale` and checked in
// `zone`.
MotionSafeAreaReport CheckScaled(std::vector<MotionEvaluatedNode> children,
                                 double scale, const MotionSafeZone& zone) {
    MotionFrame frame = Frame({});
    frame.width = static_cast<int32_t>(1080 * scale);
    frame.height = static_cast<int32_t>(1920 * scale);
    frame.root.box = {0, 0, 1080 * scale, 1920 * scale};
    for (MotionEvaluatedNode& child : children) {
        child.box = {child.box.x * scale, child.box.y * scale,
                     child.box.width * scale, child.box.height * scale};
        for (MotionTextLine& line : child.text_layout.lines) {
            line.x *= scale;
            line.width *= scale;
            line.height *= scale;
            line.y *= scale;
        }
    }
    frame.root.children = std::move(children);
    MotionSafeAreaReport report;
    report.zone = zone;
    CheckMotionFrameSafeArea(frame, report);
    return report;
}

bool Near(double value, double expected) {
    return value > expected - 1e-6 && value < expected + 1e-6;
}

void CheckSocialZone() {
    const MotionSafeZone social = SocialVerticalMotionSafeZone();
    MotionSafeZone named;
    Check(NamedMotionSafeZone("social_vertical", named) &&
              named.name == "social_vertical" && named.right.num == 140 &&
              NamedMotionSafeZone("default", named) &&
              named.name == "default" && !NamedMotionSafeZone("tiktok", named),
          "two zones are named, and only two");
    Check(
        DescribeMotionSafeZone(DefaultMotionSafeZone()) == "5 % on each side" &&
            DescribeMotionSafeZone(social) ==
                "top 14 %, bottom 35 %, left 6 %, right 12.96 %",
        "a zone reads as its margins: " + DescribeMotionSafeZone(social));

    // In proportion of the frame: 14 % at the top, 35 % at the bottom, 6 %
    // on the left, and TikTok's rail on the right, 940 of 1080 px. The same
    // rectangle at twice the size.
    for (const double scale : {1.0, 2.0}) {
        const MotionSafeAreaReport empty = CheckScaled({}, scale, social);
        const std::string size = scale == 1.0 ? "1080x1920" : "2160x3840";
        Check(Near(empty.safe.x, 64.8 * scale) &&
                  Near(empty.safe.y, 268.8 * scale) &&
                  Near(empty.safe.x + empty.safe.width, 940.0 * scale) &&
                  Near(empty.safe.y + empty.safe.height, 1248.0 * scale),
              "in " + size + " the social zone is x 64.8 to 940 and y 268.8 " +
                  "to 1248, in proportion: " +
                  SerializeMotionSafeAreaReport(empty));

        // A text in the middle of the frame passes both zones.
        const MotionEvaluatedNode middle =
            TextNode("centre", 400, 930, 280, {"Camille"});
        Check(CheckScaled({middle}, scale, social).issues.empty() &&
                  CheckScaled({middle}, scale, DefaultMotionSafeZone())
                      .issues.empty(),
              "in " + size + " a text in the middle passes both zones");

        // A name low in the frame, near where the nom piece puts its own (y
        // 1340 to 1498): inside the default zone, under the apps' caption
        // in the social one.
        const MotionEvaluatedNode low =
            TextNode("nom", 86, 1340, 400, {"Camille ", "Martin"});
        const MotionSafeAreaReport lowSocial =
            CheckScaled({low}, scale, social);
        Check(
            CheckScaled({low}, scale, DefaultMotionSafeZone()).issues.empty() &&
                lowSocial.issues.size() == 1 &&
                lowSocial.issues[0].edges == std::vector<std::string>{"bottom"},
            "in " + size +
                " a name at 70 % of the height passes the default zone and "
                "crosses the social zone's bottom");

        // The rail: a text ending at 1000 px would clear the Reels 6 %
        // (1015) and is under TikTok's buttons.
        const MotionEvaluatedNode railed =
            TextNode("droite", 860, 800, 200, {"Camille"});
        const MotionSafeAreaReport railedSocial =
            CheckScaled({railed}, scale, social);
        Check(CheckScaled({railed}, scale, DefaultMotionSafeZone())
                      .issues.empty() &&
                  railedSocial.issues.size() == 1 &&
                  railedSocial.issues[0].edges ==
                      std::vector<std::string>{"right"},
              "in " + size +
                  " a text ending at 1000 of 1080 px crosses "
                  "TikTok's rail");
    }

    // The report names the zone and gives its margins; margin_percent is
    // left out, since no single margin describes it.
    const std::string json =
        SerializeMotionSafeAreaReport(CheckScaled({}, 1.0, social));
    Check(json.find("\"zone\":\"social_vertical\",\"margins\":{\"top\":14,"
                    "\"bottom\":35,\"left\":6,\"right\":12.96}") !=
                  std::string::npos &&
              json.find("margin_percent") == std::string::npos &&
              json.find("\"safe_area\":{\"x\":65,\"y\":269,\"width\":875,"
                        "\"height\":979}") != std::string::npos,
          "the social report says which zone it applied: " + json);

    // The nom piece itself, in 9x16 and at rest (frame 80, opaque, its
    // slide long finished). Its shipped 9:16 layout, 36 % from the bottom
    // (the author, 24 October 2026), passes both zones; at its old height, 22
    // %, it passes the default zone and leaves the social one by its bottom --
    // the case of 29 September 2026.
    std::vector<MotionTemplateInfo> templates;
    std::string error;
    Check(MotionTemplateCatalog(templates, error), "catalog loads: " + error);
    const MotionTemplateInfo* nom = Find(templates, "nom");
    Value instance;
    std::string chosen;
    MotionComposition composition;
    MotionError motion;
    MotionFrame shipped;
    MotionFrame lower;
    FakeMeasurer measurer;
    Value oldHeight;
    Value::Parse(R"({"hauteur":22})", oldHeight, error);
    if (nom == nullptr ||
        !InstantiateMotionTemplate(nom->definition, 1080, 1920, instance,
                                   chosen, error) ||
        !ParseMotionComposition(instance, composition, motion) ||
        !EvaluateMotionFrame(composition, Value(), 80, measurer, shipped,
                             motion) ||
        !EvaluateMotionFrame(composition, oldHeight, 80, measurer, lower,
                             motion)) {
        Check(false, "the nom piece evaluates in 9x16: " + error + " " +
                         motion.message);
        return;
    }
    MotionSafeAreaReport shippedDefault;
    CheckMotionFrameSafeArea(shipped, shippedDefault);
    MotionSafeAreaReport shippedSocial;
    shippedSocial.zone = social;
    CheckMotionFrameSafeArea(shipped, shippedSocial);
    Check(chosen == "9x16" && shippedDefault.issues.empty() &&
              shippedSocial.issues.empty(),
          "the shipped nom piece in 9x16 passes both zones: default " +
              SerializeMotionSafeAreaReport(shippedDefault) + " / social " +
              SerializeMotionSafeAreaReport(shippedSocial));
    MotionSafeAreaReport byDefault;
    CheckMotionFrameSafeArea(lower, byDefault);
    MotionSafeAreaReport bySocial;
    bySocial.zone = social;
    CheckMotionFrameSafeArea(lower, bySocial);
    const bool allBottom =
        !bySocial.issues.empty() &&
        std::all_of(bySocial.issues.begin(), bySocial.issues.end(),
                    [](const MotionSafeAreaIssue& issue) {
                        return issue.kind == "outside_safe_area" &&
                               issue.edges ==
                                   std::vector<std::string>{"bottom"};
                    });
    Check(byDefault.issues.empty() && allBottom,
          "the nom piece at 22 % passes the default zone and leaves the "
          "social one by its bottom: default " +
              SerializeMotionSafeAreaReport(byDefault) + " / social " +
              SerializeMotionSafeAreaReport(bySocial));
}

// DEC11 -- docs/roadmap/MOTION-WEB-2026-10.md. A search of the shelf: every
// word, anywhere in what a piece says of itself, without case or accents.
void CheckShelfSearch() {
    MotionTemplateInfo piece;
    piece.name = "hf-lt-soft-pill";
    piece.description =
        "Lower Third — Soft Pill: rounded white pill. Tags: lower-third, "
        "interview.";
    piece.use = "Entrée à l'écran d'une personne qui parle.";
    piece.avoid = "Un plan très chargé.";
    piece.invariants = {"Le nom reste sur une ligne."};
    piece.origin = "HyperFrames registry (Apache-2.0)";
    struct Case {
        const char* query;
        bool matches;
        const char* why;
    };
    const std::vector<Case> cases = {
        {"", true, "an empty query matches every piece"},
        {"   ", true, "so does one of spaces"},
        {"soft-pill", true, "a part of the name"},
        {"LOWER third", true, "two words, any case, across a hyphen"},
        {"entree ECRAN", true, "the use, accents ignored"},
        {"écran", true, "an accent in the query too"},
        {"tres charge", true, "the avoid"},
        {"ligne", true, "an invariant"},
        {"apache", true, "the origin"},
        {"lower karaoke", false, "every word must be there"},
        {"titre", false, "a word that is nowhere"},
    };
    for (const Case& test : cases) {
        Check(MotionPieceMatches(piece, test.query) == test.matches,
              std::string("search '") + test.query + "': " + test.why);
    }
    // The shipped shelf, searched: one piece says it is a name on screen.
    std::vector<MotionTemplateInfo> templates;
    std::string error;
    Check(MotionTemplateCatalog(templates, error), "catalog loads: " + error);
    std::vector<std::string> found;
    for (const MotionTemplateInfo& info : templates) {
        if (MotionPieceMatches(info, "name on SCREEN")) {
            found.push_back(info.name);
        }
    }
    Check(found == std::vector<std::string>{"nom"},
          "the shipped shelf answers 'name on screen' with nom alone");
}

// DEC34 -- docs/roadmap/MOTION-2026-09.md. The zone a clip is checked in
// when nobody names one: social_vertical on a 9:16 timeline at any size,
// the default zone on every other, 4:5 and the near-vertical sizes no
// aspect rounds into 9:16 among them.
void CheckTimelineZone() {
    for (const auto& [width, height] : std::vector<std::pair<int64_t, int64_t>>{
             {1080, 1920}, {2160, 3840}, {720, 1280}, {540, 960}}) {
        const MotionSafeZone zone = TimelineMotionSafeZone(width, height);
        Check(IsNineBySixteen(width, height) &&
                  zone.name == "social_vertical" && zone.bottom.num == 35 &&
                  zone.right.num == 140 && zone.right.den == 1080,
              std::to_string(width) + "x" + std::to_string(height) +
                  " is 9:16: the social zone");
    }
    for (const auto& [width, height] :
         std::vector<std::pair<int64_t, int64_t>>{{1920, 1080},
                                                  {1080, 1080},
                                                  {1080, 1350},
                                                  {1080, 1918},
                                                  {1081, 1920},
                                                  {1080, 1921},
                                                  {0, 0},
                                                  {-1080, -1920}}) {
        const MotionSafeZone zone = TimelineMotionSafeZone(width, height);
        Check(!IsNineBySixteen(width, height) && zone.name == "default" &&
                  zone.top.num == kMotionSafeMarginPercent &&
                  zone.right.num == kMotionSafeMarginPercent,
              std::to_string(width) + "x" + std::to_string(height) +
                  " is not 9:16: the default zone");
    }
}

// MM3 -- docs/roadmap/MOTION-MARQUE-2026-10.md. A composition checked alone
// is often read from a file: its nodes have neither id nor ULID, and are
// told apart by their path. A line the text breaks itself is no split
// word. And what crosses the edge only while it moves is a passage, which
// `at_rest_only` leaves out.
void CheckSafeAreaAtRest() {
    MotionEvaluatedNode figures =
        TextNode("chiffres", 100, 300, 400, {"12 480", "9 731"});
    figures.text = "12 480\n9 731";
    MotionSafeAreaReport report = CheckOne(Frame({figures}));
    Check(report.issues.empty(),
          "a line the text breaks itself is no split word: " +
              SerializeMotionSafeAreaReport(report));

    MotionEvaluatedNode one = TextNode("", 1000, 300, 400, {"Un"});
    MotionEvaluatedNode two = TextNode("", 1000, 600, 400, {"Deux"});
    one.uid.clear();
    two.uid.clear();
    one.path = "root.children[0]";
    two.path = "root.children[1]";
    report = CheckOne(Frame({one, two}));
    const std::string both = SerializeMotionSafeAreaReport(report);
    Check(report.issues.size() == 2 &&
              both.find("\"path\":\"root.children[1]\"") != std::string::npos,
          "two anonymous texts are two issues, each by its path: " + both);

    // A title that slides out over the right edge and stops there, and one
    // that slides through and goes on: both inside at frame 0 (a title is
    // 100 px wide), outside from frame 1.
    const auto title = [](const std::string& id, double x, double y) {
        return TextNode(id, x, y, 400, {"Titre"});
    };
    const std::vector<std::vector<double>> places = {{900, 950, 950},
                                                     {900, 950, 1000}};
    for (const bool restOnly : {false, true}) {
        MotionSafeAreaReport checked;
        checked.at_rest_only = restOnly;
        for (int64_t frame = 0; frame < 3; ++frame) {
            CheckMotionFrameSafeArea(
                Frame({title("arret", places[0][frame], 300),
                       title("passe", places[1][frame], 700)},
                      frame),
                checked);
        }
        const std::string json = SerializeMotionSafeAreaReport(checked);
        const MotionSafeAreaIssue* stopped = nullptr;
        const MotionSafeAreaIssue* passing = nullptr;
        for (const MotionSafeAreaIssue& issue : checked.issues) {
            (issue.node_id == "arret" ? stopped : passing) = &issue;
        }
        if (!restOnly) {
            Check(stopped != nullptr && stopped->at_rest &&
                      passing != nullptr && !passing->at_rest &&
                      json.find("\"at_rest\":true") != std::string::npos,
                  "every crossing is reported, each says whether it stood "
                  "still: " +
                      json);
        } else {
            Check(stopped != nullptr && stopped->first_frame == 2 &&
                      passing == nullptr && checked.in_motion == 3 &&
                      json.find("\"in_motion\":3") != std::string::npos,
                  "at rest only: the title held outside, from the frame it "
                  "stops; the passages counted apart: " +
                      json);
        }
    }
}

// MB1 -- a piece with a name on a band, in two formats.
const char kBandComposition[] =
    R"({"width":1080,"height":1920,"fps":25,"duration":50,)"
    R"("props":{"mot":{"type":"text","default":"Bonjour"},)"
    R"("corps":{"type":"number","min":16,"max":400,"default":120}},)"
    R"("formats":{"9x16":{"width":1080,"height":1920},)"
    R"("16x9":{"width":1920,"height":1080,"props":{"corps":90}}},)"
    R"("root":{"type":"fill","children":[{"type":"text","id":"mot",)"
    R"("content":{"prop":"mot"},"style":{"fontSize":{"prop":"corps"}},)"
    R"("animate":{"translateY":{"interpolate":{"frames":[0,12],)"
    R"("values":[80,0],"easing":"ease-out"}}}}]}})";

std::string PieceFile(const std::string& fields,
                      const std::string& composition = kBandComposition) {
    return R"({"format":"motion-piece.v1",)" + fields + R"("composition":)" +
           composition + "}";
}

bool Refused(const std::string& json, const std::string& reason,
             std::string& error) {
    MotionTemplateInfo piece;
    return !ParseMotionPiece("bande", json, piece, error) &&
           error.find(reason) != std::string::npos;
}

void CheckPieceFile() {
    MotionTemplateInfo piece;
    std::string error;
    Check(ParseMotionPiece(
              "bande",
              PieceFile(R"("description":"Un mot sur une bande.",)"
                        R"("use":"Un mot fort.","avoid":"Une phrase.",)"
                        R"("invariants":["Le mot entre par le bas."],)"
                        R"("origin":"essai",)"),
              piece, error) &&
              piece.name == "bande" &&
              piece.description == "Un mot sur une bande." &&
              piece.use == "Un mot fort." && piece.avoid == "Une phrase." &&
              piece.invariants ==
                  std::vector<std::string>{"Le mot entre par le bas."} &&
              piece.origin == "essai" && piece.path.empty() &&
              piece.definition.Find("root") != nullptr,
          "a piece file reads with what it says of itself: " + error);

    // What its file spells reads back the same, field for field and byte
    // for byte of the composition: the shelf loses nothing.
    piece.description = "Guillemets \" et accents é, sur\ndeux lignes.";
    piece.invariants.push_back(
        "Une seconde phrase, assez longue pour ne pas tenir sur la ligne du "
        "tableau.");
    const std::string file = DumpMotionPiece(piece);
    MotionTemplateInfo again;
    Check(ParseMotionPiece("bande", file, again, error) &&
              again.description == piece.description &&
              again.use == piece.use && again.avoid == piece.avoid &&
              again.invariants == piece.invariants &&
              again.origin == piece.origin &&
              again.definition.Dump() == piece.definition.Dump(),
          "a dumped piece reads back the same: " + error + "\n" + file);
    Check(file.find("\"frames\": [0, 12]") != std::string::npos &&
              file.find("\n  \"invariants\": [\n    \"") != std::string::npos &&
              !file.empty() && file.back() == '\n',
          "the file keeps numbers on a line and sentences apart:\n" + file);

    const std::string described = R"("description":"Une bande.",)";
    Check(
        Refused("[1]", "JSON object", error) && Refused("{", "not JSON", error),
        "what is not a JSON object is refused: " + error);
    Check(Refused(kBandComposition, "unknown field 'width'", error),
          "a bare composition is refused, by its first field: " + error);
    Check(Refused(R"({"format":"motion-piece.v2","description":"x",)"
                  R"("composition":)" +
                      std::string(kBandComposition) + "}",
                  "motion-piece.v1", error),
          "another format is refused, naming the one read: " + error);
    Check(Refused(PieceFile(""), "'description' is required", error) &&
              Refused(PieceFile(R"("description":"",)"),
                      "'description' is required", error),
          "a piece without a description is refused: " + error);
    Check(Refused(PieceFile(described + R"("tags":["a"],)"),
                  "unknown field 'tags'", error),
          "a field the format does not know is refused: " + error);
    Check(Refused(PieceFile(described + R"("invariants":"une phrase",)"),
                  "array of sentences", error) &&
              Refused(PieceFile(described + R"("invariants":[""],)"),
                      "array of sentences", error),
          "invariants are sentences: " + error);
    Check(Refused(PieceFile(described,
                            R"({"width":1080,"height":1920,"fps":25,)"
                            R"("duration":50,"root":{"type":"cercle"}})"),
                  "not a composition", error),
          "a composition the parser refuses is refused: " + error);
    Check(Refused(
              PieceFile(described, R"({"width":1080,"height":1920,"fps":25,)"
                                   R"("duration":50,"root":{"type":"fill"}})"),
              "declares no format", error),
          "a piece no timeline could take is refused: " + error);
    Check(Refused(PieceFile(described,
                            R"({"width":1080,"height":1920,"fps":25,)"
                            R"("duration":50,"formats":{"9x16":{"width":1080,)"
                            R"("height":1920,"props":{"absente":1}}},)"
                            R"("root":{"type":"fill"}})"),
                  "UnknownProp at formats.9x16.props.absente", error),
          "a format that sets a prop the piece lacks is refused: " + error);
    Value band;
    Value::Parse(kBandComposition, band, error);
    Check(Refused(PieceFile(described, WithDefinitionIdentities(band).Dump()),
                  "carries identities", error),
          "a composition taken from a document as it is, is refused: " + error);

    Check(IsMotionPieceName("bande") && IsMotionPieceName("Fente-2_b") &&
              IsMotionPieceName("9x16") && !IsMotionPieceName("") &&
              !IsMotionPieceName("-bande") && !IsMotionPieceName("../nom") &&
              !IsMotionPieceName("a.b") && !IsMotionPieceName("é") &&
              !IsMotionPieceName(std::string(65, 'a')),
          "a piece name is a file name, never a path");
}

void WriteFile(const std::filesystem::path& path, const std::string& text) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << text;
}

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(file)),
                       std::istreambuf_iterator<char>());
}

void CheckLibrary() {
    namespace fs = std::filesystem;
    const fs::path directory =
        fs::temp_directory_path() /
        ("cutmachine-motion-library-" + std::to_string(::getpid()));
    fs::remove_all(directory);
    std::vector<MotionTemplateInfo> shipped;
    std::string error;
    Check(MotionTemplateCatalog(shipped, error), "catalog loads: " + error);

    std::vector<MotionTemplateInfo> pieces;
    std::vector<MotionLibraryIssue> issues;
    ReadMotionLibrary(directory.string(), shipped, pieces, issues);
    Check(pieces.empty() && issues.empty(),
          "a library that does not exist yet is empty, not an error");
    ReadMotionLibrary("", shipped, pieces, issues);
    Check(pieces.empty() && issues.empty(), "no directory, no library");

    fs::create_directories(directory / "sous-dossier");
    const std::string described = R"("description":"Une bande.",)";
    WriteFile(directory / "zebre.json", PieceFile(described));
    WriteFile(directory / "bande.json", PieceFile(described));
    WriteFile(directory / "cassee.json", "{");
    WriteFile(directory / "nom.json", PieceFile(described));
    WriteFile(directory / "mal nommee.json", PieceFile(described));
    WriteFile(directory / "notes.txt", "pas une pièce");
    WriteFile(directory / ".bande.json.tmp-1", "{");
    WriteFile(directory / "sous-dossier" / "cachee.json", PieceFile(described));
    ReadMotionLibrary(directory.string(), shipped, pieces, issues);
    Check(pieces.size() == 2 && pieces[0].name == "bande" &&
              pieces[1].name == "zebre" &&
              pieces[0].path == (directory / "bande.json").string(),
          "the library lists its pieces by name, each with its file");
    const auto issueOf = [&](const std::string& file) {
        for (const MotionLibraryIssue& issue : issues) {
            if (issue.path == (directory / file).string()) {
                return issue.message;
            }
        }
        return std::string();
    };
    Check(issues.size() == 3 &&
              issueOf("cassee.json").find("not JSON") != std::string::npos &&
              issueOf("nom.json").find("ships") != std::string::npos &&
              issueOf("mal nommee.json").find("not a piece name") !=
                  std::string::npos,
          "a broken file, a shipped name and a bad name are reported and "
          "nothing else is: " +
              std::to_string(issues.size()) + " issue(s)");

    // Back to the shelf. A composition as a document stores it: identified,
    // and at one size.
    Value band;
    Value::Parse(kBandComposition, band, error);
    Value single = Value::MakeObject();
    for (const auto& [key, value] : band.AsObject()) {
        if (key != "formats") {
            single.Set(key, value);
        }
    }
    const Value stored = WithDefinitionIdentities(single);
    std::vector<Ulid> nodes;
    std::vector<Ulid> keyframes;
    MotionDefinitionIdentities(stored, nodes, keyframes);
    Check(nodes.size() == 2 && keyframes.size() == 2,
          "the stored composition is identified");
    MotionTemplateInfo piece;
    piece.name = "retour";
    piece.description = "Une bande revenue d'un projet.";
    Check(MotionPieceDefinition(stored, piece.definition, error),
          "a stored composition makes a piece: " + error);
    MotionDefinitionIdentities(piece.definition, nodes = {}, keyframes = {});
    const Value* formats = piece.definition.Find("formats");
    Check(
        nodes.empty() && keyframes.empty() && formats != nullptr &&
            formats->Find("9x16") != nullptr &&
            formats->Find("9x16")->Dump() == R"({"width":1080,"height":1920})",
        "it leaves its identities and gains a format of its own size: " +
            piece.definition.Dump());
    // Apart from that, nothing of it changed.
    Value withoutFormats = Value::MakeObject();
    for (const auto& [key, value] : piece.definition.AsObject()) {
        if (key != "formats") {
            withoutFormats.Set(key, value);
        }
    }
    Check(withoutFormats.Dump() == single.Dump(),
          "and is otherwise the composition as it was written");

    // One that already declares its own size keeps its formats as they are.
    Value kept;
    Check(MotionPieceDefinition(WithDefinitionIdentities(band), kept, error) &&
              kept.Dump() == band.Dump(),
          "a composition with a format of its size is only anonymised");
    // The usual name taken by another size: the size names the format.
    Value clash;
    Value::Parse(R"({"width":1080,"height":1920,"fps":25,"duration":50,)"
                 R"("formats":{"9x16":{"width":720,"height":1280}},)"
                 R"("root":{"type":"fill"}})",
                 clash, error);
    Value named;
    Check(MotionPieceDefinition(clash, named, error) &&
              named.Find("formats")->Find("1080x1920") != nullptr &&
              named.Find("formats")->Find("9x16")->Dump() ==
                  R"({"width":720,"height":1280})",
          "a taken format name is left alone: " + named.Dump());

    // What belongs to the project stays there.
    for (
        const char* bound :
        {R"({"type":"video","id":"plan","media":"01K40000000000000000000001"})",
         R"({"type":"audio","media":"01K40000000000000000000001"})",
         R"({"type":"composition","composition":"01K40000000000000000000002"})"}) {
        Value holder;
        Value::Parse(std::string(R"({"width":1080,"height":1920,"fps":25,)") +
                         R"("duration":50,"root":{"type":"fill","children":[)" +
                         R"({"type":"box","children":[)" + bound + "]}]}}",
                     holder, error);
        Value refused;
        Check(!MotionPieceDefinition(holder, refused, error) &&
                  error.find("of this project") != std::string::npos,
              std::string("a node of the project is refused: ") + bound +
                  " -> " + error);
    }
    {
        Value holder;
        Value::Parse(
            R"({"width":1080,"height":1920,"fps":25,"duration":50,)"
            R"("root":{"type":"fill","children":[{"type":"video",)"
            R"("src":"plan.mov"},{"type":"repeat","count":2,"template":)"
            R"({"type":"video","id":"rush","media":"01K4"}}]}})",
            holder, error);
        Value refused;
        Check(!MotionPieceDefinition(holder, refused, error) &&
                  error.find("'rush' (video)") != std::string::npos &&
                  error.find("plan.mov") == std::string::npos,
              "a file played by `src` passes, a media under a repeat does "
              "not, and is named: " +
                  error);
    }
    {
        // A media named by a prop is the piece's to carry: every project
        // that adds it says which of its own it plays.
        Value holder;
        Value::Parse(R"({"width":1080,"height":1920,"fps":25,"duration":50,)"
                     R"("props":{"plan":{"type":"text","default":""}},)"
                     R"("root":{"type":"fill","children":[{"type":"video",)"
                     R"("media":{"prop":"plan"}}]}})",
                     holder, error);
        Value carried;
        Check(MotionPieceDefinition(holder, carried, error) &&
                  carried.Find("formats") != nullptr,
              "a media bound to a prop passes: " + error);
    }

    // Written whole, refused over an existing piece, replaced on demand.
    std::string path;
    bool existed = true;
    Check(WriteMotionPiece(directory.string(), piece, false, path, existed,
                           error) &&
              !existed && path == (directory / "retour.json").string() &&
              ReadFile(path) == DumpMotionPiece(piece),
          "a piece is written to its file: " + error);
    Check(!WriteMotionPiece(directory.string(), piece, false, path, existed,
                            error) &&
              existed && error.find("overwrite") != std::string::npos,
          "a second piece of that name is refused: " + error);
    piece.description = "Une bande, seconde version.";
    Check(WriteMotionPiece(directory.string(), piece, true, path, existed,
                           error) &&
              existed && ReadFile(path) == DumpMotionPiece(piece),
          "and replaces it when asked: " + error);
    piece.name = "../dehors";
    Check(!WriteMotionPiece(directory.string(), piece, true, path, existed,
                            error) &&
              !fs::exists(directory.parent_path() / "dehors.json"),
          "a name that is a path writes nothing: " + error);
    Check(!WriteMotionPiece("", piece, true, path, existed, error),
          "no directory, nothing written");

    ReadMotionLibrary(directory.string(), shipped, pieces, issues);
    const MotionTemplateInfo* back = Find(pieces, "retour");
    Check(pieces.size() == 3 && issues.size() == 3 && back != nullptr &&
              back->description == "Une bande, seconde version.",
          "the saved piece is on the shelf, and no temporary file with it");
    // And a 9:16 timeline takes it back, with fresh identities to come.
    Value instance;
    std::string format;
    Check(back != nullptr &&
              InstantiateMotionTemplate(back->definition, 1080, 1920, instance,
                                        format, error) &&
              format == "9x16" && instance.Find("formats") == nullptr &&
              instance.Dump() == single.Dump(),
          "the piece instantiates as the composition it was saved from: " +
              error);
    Check(back != nullptr &&
              !InstantiateMotionTemplate(back->definition, 1920, 1080, instance,
                                         format, error),
          "and has no layout it was not given");

    // The directory is created with its parents on the first save.
    const fs::path nested = directory / "plus" / "loin";
    piece.name = "premiere";
    Check(
        WriteMotionPiece(nested.string(), piece, false, path, existed, error) &&
            fs::exists(nested / "premiere.json"),
        "the first save creates the library: " + error);
    fs::remove_all(directory);
}

// MB4 -- docs/roadmap/MOTION-BIBLIOTHEQUE-2026-10.md. A charter as data:
// the kit file and its refusals, the kits read beside the pieces, and one
// shipped piece taking two charters in every format without its JSON being
// written by hand.
std::string DefaultOf(const Value& definition, const std::string& prop) {
    const Value* props = definition.Find("props");
    const Value* entry = props != nullptr ? props->Find(prop) : nullptr;
    const Value* value = entry != nullptr ? entry->Find("default") : nullptr;
    return value != nullptr && value->IsString() ? value->AsString() : "";
}

void CheckKits() {
    namespace fs = std::filesystem;
    MotionKitInfo kit;
    std::string error;
    const auto refused = [&](const std::string& json,
                             const std::string& reason) {
        return !ParseMotionKit("k", json, kit, error) &&
               error.find(reason) != std::string::npos;
    };
    Check(refused("{", "not JSON"), "a kit that is not JSON: " + error);
    Check(refused(R"({"format":"motion-piece.v1","description":"x",)"
                  R"("props":{"accent":"#000000"}})",
                  "'format' must be \"motion-kit.v1\""),
          "a piece is not a kit: " + error);
    Check(refused(R"({"format":"motion-kit.v1","props":{"accent":"#000"}})",
                  "'description' is required"),
          "a kit says what it is: " + error);
    Check(refused(R"({"format":"motion-kit.v1","description":"x","props":{}})",
                  "'props' is required"),
          "a kit gives values: " + error);
    Check(refused(R"({"format":"motion-kit.v1","description":"x",)"
                  R"("props":{"couleur":"#000000"}})",
                  "'props.couleur' is not a prop pieces share; a kit sets "
                  "accent, texte, fond, police"),
          "only the shared names, listed when one is wrong: " + error);
    Check(refused(R"({"format":"motion-kit.v1","description":"x",)"
                  R"("props":{"accent":"bleu roi"}})",
                  "'props.accent' must be a color"),
          "a color the motion format cannot read: " + error);
    Check(refused(R"({"format":"motion-kit.v1","description":"x",)"
                  R"("props":{"police":""}})",
                  "'props.police' must be a non-empty text"),
          "a font family is named: " + error);
    Check(refused(R"({"format":"motion-kit.v1","description":"x",)"
                  R"("logo":"logo.png","props":{"accent":"#000000"}})",
                  "unknown field 'logo'"),
          "a field the format does not know: " + error);

    const fs::path library =
        fs::temp_directory_path() /
        ("cutmachine-motion-kits-" + std::to_string(::getpid()));
    fs::remove_all(library);
    std::vector<MotionKitInfo> kits;
    std::vector<MotionLibraryIssue> issues;
    ReadMotionKits(library.string(), kits, issues);
    Check(kits.empty() && issues.empty(), "no kits folder, no kit");
    fs::create_directories(library / "kits");
    WriteFile(library / "kits" / "bleu.json",
              R"({"format":"motion-kit.v1","description":"Charte bleue.",)"
              R"("principle":"Une barre sous chaque titre.",)"
              R"("props":{"accent":"#0000FF","texte":"#FFFFFF",)"
              R"("fond":"#101010","police":"Avenir Next"}})");
    WriteFile(library / "kits" / "rose.json",
              R"({"format":"motion-kit.v1","description":"Charte rose.",)"
              R"("props":{"accent":"#E0A030","police":"Helvetica"}})");
    WriteFile(library / "kits" / "cassee.json", "{");
    WriteFile(library / "bande.json", PieceFile(R"("description":"x",)"));
    ReadMotionKits(library.string(), kits, issues);
    Check(kits.size() == 2 && kits[0].name == "bleu" &&
              kits[0].principle == "Une barre sous chaque titre." &&
              kits[1].name == "rose" && issues.size() == 1 &&
              issues[0].path == (library / "kits" / "cassee.json").string(),
          "the kits of the library by name, a broken one named apart");
    std::vector<MotionTemplateInfo> pieces;
    std::vector<MotionTemplateInfo> shipped;
    Check(MotionTemplateCatalog(shipped, error), "catalog loads: " + error);
    ReadMotionLibrary(library.string(), shipped, pieces, issues);
    Check(pieces.size() == 1 && issues.empty(),
          "the kits folder is not a piece of the library");

    // One piece, two charters, every format.
    const MotionTemplateInfo* carton = Find(shipped, "carton");
    Check(carton != nullptr &&
              DefaultOf(carton->definition, "police") == "Helvetica Neue",
          "the shipped card names its font in a prop, Helvetica Neue");
    if (carton == nullptr || kits.size() != 2) {
        fs::remove_all(library);
        return;
    }
    Value blue = carton->definition;
    MotionKitApplication application;
    Check(
        ApplyMotionKit(kits[0], "carton", blue, application, error) &&
            application.applied ==
                std::vector<std::string>{"texte", "fond", "police"} &&
            application.not_in_piece == std::vector<std::string>{"accent"},
        "the card takes the colours and the font, and has no accent: " + error);
    for (const auto& [width, height] :
         std::vector<std::pair<int32_t, int32_t>>{{1080, 1920}, {1920, 1080}}) {
        Value instance;
        std::string format;
        Check(InstantiateMotionTemplate(blue, width, height, instance, format,
                                        error) &&
                  DefaultOf(instance, "fond") == "#101010" &&
                  DefaultOf(instance, "texte") == "#FFFFFF" &&
                  DefaultOf(instance, "police") == "Avenir Next" &&
                  DefaultOf(instance, "titre") == "Titre",
              "the blue card in " + format + ": " + error);
    }
    Value pink = carton->definition;
    Check(ApplyMotionKit(kits[1], "carton", pink, application, error) &&
              DefaultOf(pink, "police") == "Helvetica" &&
              DefaultOf(pink, "fond") == "#000000" &&
              pink.Dump() != blue.Dump(),
          "the same card with the other kit is another charter: " + error);
    Check(carton->definition.Find("props") != nullptr &&
              DefaultOf(carton->definition, "police") == "Helvetica Neue",
          "the shelf's piece is left as it was");

    // A format that sets a shared prop takes the kit's value; a variant's
    // own value stays.
    Value own;
    Check(Value::Parse(
              R"({"width":100,"height":100,"fps":25,"duration":10,)"
              R"("props":{"accent":{"type":"color","default":"#000000"}},)"
              R"("formats":{"a":{"width":100,"height":100,)"
              R"("props":{"accent":"#111111"}}},)"
              R"("variants":{"v":{"props":{"accent":"#222222"}}},)"
              R"("root":{"type":"fill"}})",
              own, error),
          "a composition with a format and a variant: " + error);
    Check(ApplyMotionKit(kits[1], "propre", own, application, error) &&
              own.Dump().find(R"("formats":{"a":{"width":100,"height":100,)"
                              R"("props":{"accent":"#E0A030"}}})") !=
                  std::string::npos &&
              own.Dump().find(R"("accent":"#222222")") != std::string::npos,
          "the format takes the charter, the variant keeps its own: " +
              own.Dump());
    // A piece whose shared name holds something else does not follow the
    // convention: refused rather than fed a color as a font.
    Value odd;
    Value::Parse(R"({"props":{"police":{"type":"color","default":"#000000"}}})",
                 odd, error);
    Check(
        !ApplyMotionKit(kits[1], "etrange", odd, application, error) &&
            error.find("does not follow the shared names") != std::string::npos,
        "a shared name of another type is refused: " + error);
    fs::remove_all(library);
}

// DEC6 -- docs/roadmap/MOTION-BIBLIOTHEQUE-2026-10.md. The kit grown into a
// brand's charter: its fields, what it allows, the caption style it gives
// at each size, and every refusal by name.
void CheckBrandKits() {
    MotionKitInfo kit;
    std::string error;
    const auto refused = [&](const std::string& json,
                             const std::string& reason) {
        return !ParseMotionKit("k", json, kit, error) &&
               error.find(reason) != std::string::npos;
    };
    const std::string head =
        R"({"format":"brand-kit.v1","description":"Charte bleue.",)";
    Check(refused(R"({"format":"motion-kit.v1","description":"x",)"
                  R"("props":{"accent":"#000000"},"palette":["#000000"]})",
                  "unknown field 'palette': a field of a brand's charter, "
                  "write \"format\": \"brand-kit.v1\""),
          "a motion-kit.v1 file does not half-read a charter: " + error);
    Check(refused(head + R"("palette":["#00f"]})", "'palette[0]' must be"),
          "a palette lists #rrggbb: " + error);
    Check(refused(head + R"("fonts":[""]})", "'fonts[0]' must be a non-empty"),
          "a font is named: " + error);
    Check(refused(head + R"("captions":{"color":"bleu"}})",
                  "'captions.color' must be \"#rrggbb\""),
          "a caption colour both FFmpeg and the check read: " + error);
    Check(refused(head + R"("captions":{"taille":40}})",
                  "'captions.taille' is not a caption setting"),
          "a caption setting the style does not have: " + error);
    Check(refused(head + R"("captions":{"font_size":0}})",
                  "'captions' has an invalid font_family, font_size"),
          "a value the caption style refuses: " + error);
    Check(refused(head + R"("captions":{"color":"#FFFFFF","formats":)"
                         R"({"9x16":{"font_size":72}}}})",
                  "'captions.formats.9x16' needs its size"),
          "a caption format says its size: " + error);
    Check(refused(head + R"("captions":{"color":"#FFFFFF","formats":)"
                         R"({"a":{"width":10,"height":10},)"
                         R"("b":{"width":10,"height":10}}}})",
                  "repeats the size 10x10"),
          "two caption formats of one size: " + error);
    Check(refused(head + R"("captions":{}})", "'captions' sets nothing"),
          "empty captions: " + error);
    Check(refused(head + R"("sources":["x"]})", "a charter sets something"),
          "a charter of sources alone: " + error);
    Check(refused(head + R"("props":{}})", "'props' must be an object that"),
          "empty props in a charter: " + error);

    const std::string charter =
        head +
        R"("principle":"Une barre au-dessus d'un mot.",)"
        R"("props":{"accent":"#0000FF","texte":"#FFFFFF","police":"Avenir Next"},)"
        R"("palette":["#E0A030","#000000"],"fonts":["Helvetica Neue"],)"
        R"("captions":{"font_family":"Avenir Next","color":"#FFFFFF",)"
        R"("background_color":"#0000FF@0.8","background_padding":12,)"
        R"("vertical_position":{"num":3,"den":4},"formats":{)"
        R"("9x16":{"width":1080,"height":1920,"font_size":72},)"
        R"("16x9":{"width":1920,"height":1080,"font_size":56,)"
        R"("vertical_position":{"num":5,"den":6}}}},)"
        R"("sources":["Charte 2024, p. 3"]})";
    Check(ParseMotionKit("bleu", charter, kit, error) &&
              kit.format == kBrandKitFormat && kit.palette.size() == 2 &&
              kit.sources == std::vector<std::string>{"Charte 2024, p. 3"},
          "a brand's charter reads: " + error);
    Check(
        MotionKitColors(kit) == std::vector<std::string>{"#0000ff", "#ffffff",
                                                         "#e0a030", "#000000"},
        "its colours: props, palette, captions, once each, alpha aside");
    Check(MotionKitFonts(kit) ==
              std::vector<std::string>{"Avenir Next", "Helvetica Neue"},
          "its fonts: police, fonts, the captions' family");
    Check(MotionColorKey("#0000FF") == "#0000ff" &&
              MotionColorKey("rgba(0, 0, 255, 0.5)") == "#0000ff" &&
              MotionColorKey("transparent").empty() &&
              MotionColorKey("bleu").empty(),
          "a colour compared by its RGB; transparent is no colour");

    CaptionStyle style;
    style.id = "01KA0000000000000000000099";
    std::string format;
    std::vector<std::string> set;
    Check(MotionKitCaptionStyle(kit, 1080, 1920, style, format, set, error) &&
              format == "9x16" && style.font_size == 72 &&
              style.font_family == "Avenir Next" &&
              style.background_color == "#0000FF@0.8" &&
              style.vertical_position.num == 3 &&
              style.vertical_position.den == 4 &&
              style.id == "01KA0000000000000000000099" &&
              std::find(set.begin(), set.end(), "font_size") != set.end(),
          "the 9:16 caption style: the base and its format: " + error);
    Check(MotionKitCaptionStyle(kit, 1920, 1080, style, format, set, error) &&
              format == "16x9" && style.font_size == 56 &&
              style.vertical_position.num == 5,
          "the 16:9 one: " + error);
    Check(!MotionKitCaptionStyle(kit, 1080, 1350, style, format, set, error) &&
              error.find("has captions for 1080x1920 (9x16), 1920x1080 "
                         "(16x9), none for 1080x1350") != std::string::npos,
          "a size the charter has no layout for is refused: " + error);
    MotionKitInfo motionOnly;
    Check(ParseMotionKit("v1",
                         R"({"format":"motion-kit.v1","description":"x",)"
                         R"("props":{"accent":"#000000"}})",
                         motionOnly, error) &&
              motionOnly.format == kMotionKitFormat &&
              !MotionKitCaptionStyle(motionOnly, 1080, 1920, style, format, set,
                                     error) &&
              error.find("says nothing of captions") != std::string::npos,
          "a motion-kit.v1 file still reads, and gives no captions: " + error);
    // The piece side is unchanged: the charter's props, as a kit's.
    std::vector<MotionTemplateInfo> shipped;
    Check(MotionTemplateCatalog(shipped, error), "catalog loads: " + error);
    const MotionTemplateInfo* carton = Find(shipped, "carton");
    MotionKitApplication application;
    if (carton != nullptr) {
        Value piece = carton->definition;
        Check(ApplyMotionKit(kit, "carton", piece, application, error) &&
                  DefaultOf(piece, "police") == "Avenir Next" &&
                  DefaultOf(piece, "texte") == "#FFFFFF",
              "a charter dresses a piece as a kit does: " + error);
    }
}

// MB10 -- docs/roadmap/MOTION-BIBLIOTHEQUE-2026-10.md. The library in the
// user's personal base: the inbox imported once per content, a changed file
// as the next version, a save that keeps the versions before it, rows that
// never change, and a base that cannot be used falling back to the inbox.
int64_t VersionCount(const std::string& base, const std::string& name) {
    sqlite3* db = nullptr;
    sqlite3_open_v2(base.c_str(), &db, SQLITE_OPEN_READONLY, nullptr);
    sqlite3_stmt* statement = nullptr;
    sqlite3_prepare_v2(db,
                       "SELECT count(*) FROM motion_library WHERE name = ?1",
                       -1, &statement, nullptr);
    sqlite3_bind_text(statement, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    const int64_t count = sqlite3_step(statement) == SQLITE_ROW
                              ? sqlite3_column_int64(statement, 0)
                              : -1;
    sqlite3_finalize(statement);
    sqlite3_close(db);
    return count;
}

bool Writes(const std::string& base, const char* sql) {
    sqlite3* db = nullptr;
    sqlite3_open_v2(base.c_str(), &db, SQLITE_OPEN_READWRITE, nullptr);
    const bool done =
        sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
    sqlite3_close(db);
    return done;
}

void CheckLibraryBase() {
    namespace fs = std::filesystem;
    const fs::path inbox =
        fs::temp_directory_path() /
        ("cutmachine-motion-base-" + std::to_string(::getpid()));
    fs::remove_all(inbox);
    const MotionLibraryLocation location{(inbox / "library.sqlite").string(),
                                         inbox.string()};
    std::vector<MotionTemplateInfo> shipped;
    std::string error;
    Check(MotionTemplateCatalog(shipped, error), "catalog loads: " + error);

    MotionLibraryContents contents;
    ReadMotionLibraryBase(location, shipped, contents);
    Check(contents.pieces.empty() && contents.kits.empty() &&
              contents.issues.empty() && !fs::exists(location.base),
          "an empty library is read without a base being left behind");

    fs::create_directories(inbox / "kits");
    WriteFile(inbox / "bande.json",
              PieceFile(R"("description":"Une bande.",)"));
    WriteFile(inbox / "nom.json", PieceFile(R"("description":"Un nom.",)"));
    WriteFile(inbox / "cassee.json", "{");
    WriteFile(inbox / "kits" / "bleu.json",
              R"({"format":"motion-kit.v1","description":"Bleu.",)"
              R"("props":{"accent":"#0000FF"}})");
    ReadMotionLibraryBase(location, shipped, contents);
    Check(contents.pieces.size() == 1 && contents.pieces[0].name == "bande" &&
              contents.pieces[0].version == 1 && contents.kits.size() == 1 &&
              contents.kits[0].name == "bleu" &&
              contents.kits[0].version == 1 && contents.imported.size() == 2 &&
              contents.issues.size() == 2 && fs::exists(location.base),
          "the inbox enters the base as first versions, the rest is named");
    ReadMotionLibraryBase(location, shipped, contents);
    Check(contents.imported.empty() && contents.pieces.size() == 1 &&
              contents.pieces[0].version == 1 &&
              VersionCount(location.base, "bande") == 1,
          "a file the base holds already is not imported twice");

    WriteFile(inbox / "bande.json",
              PieceFile(R"("description":"Une bande, refaite.",)"));
    ReadMotionLibraryBase(location, shipped, contents);
    Check(contents.pieces.size() == 1 && contents.pieces[0].version == 2 &&
              contents.pieces[0].description == "Une bande, refaite." &&
              contents.imported.size() == 1,
          "a changed file is the next version");

    MotionTemplateInfo piece = contents.pieces[0];
    piece.description = "Une bande, rangee par l'outil.";
    int64_t version = 0;
    bool existed = false;
    Check(!SaveMotionPieceVersion(location, piece, false, version, existed,
                                  error) &&
              existed && error.find("(v2)") != std::string::npos,
          "a name in the library is not saved over unasked: " + error);
    Check(SaveMotionPieceVersion(location, piece, true, version, existed,
                                 error) &&
              existed && version == 3,
          "asked, the save is the next version: " + error);
    ReadMotionLibraryBase(location, shipped, contents);
    Check(contents.pieces.size() == 1 && contents.pieces[0].version == 3 &&
              contents.pieces[0].description ==
                  "Une bande, rangee par l'outil." &&
              contents.imported.empty() &&
              VersionCount(location.base, "bande") == 3,
          "the latest version is listed, every version is kept, and the "
          "file (now an older version) is not imported again");
    Check(!Writes(location.base,
                  "UPDATE motion_library SET source = 'x' WHERE version = 1") &&
              !Writes(location.base, "DELETE FROM motion_library"),
          "a version never changes once written");

    // A base that is not a library: the inbox is still on the shelf.
    const MotionLibraryLocation odd{(inbox / "kits" / "bleu.json").string(),
                                    inbox.string()};
    ReadMotionLibraryBase(odd, shipped, contents);
    Check(contents.pieces.size() == 1 && contents.pieces[0].version == 0 &&
              contents.pieces[0].description == "Une bande, refaite." &&
              std::any_of(contents.issues.begin(), contents.issues.end(),
                          [&](const MotionLibraryIssue& issue) {
                              return issue.path == odd.base &&
                                     issue.message.find("cannot be used") !=
                                         std::string::npos;
                          }),
          "a base that cannot be used falls back to the inbox, and says so");

    // Where the library is: a folder named by the setting holds both.
    const char* before = std::getenv("CUTMACHINE_MOTION_LIBRARY");
    const std::string saved = before != nullptr ? before : "";
    ::setenv("CUTMACHINE_MOTION_LIBRARY", inbox.c_str(), 1);
    const MotionLibraryLocation configured = CurrentMotionLibraryLocation();
    if (before != nullptr) {
        ::setenv("CUTMACHINE_MOTION_LIBRARY", saved.c_str(), 1);
    } else {
        ::unsetenv("CUTMACHINE_MOTION_LIBRARY");
    }
    Check(configured.inbox == inbox.string() &&
              configured.base == (inbox / "library.sqlite").string(),
          "CUTMACHINE_MOTION_LIBRARY names the folder of the base and inbox");
    fs::remove_all(inbox);
}

}  // namespace

// M5b -- docs/roadmap/MOTION-2026-09.md. The shipped piece `name`, in the
// format of `width` x `height`, at `frame`, with `propsJson` over its
// defaults, as the check reads it.
bool EvaluateShipped(const std::string& name, int32_t width, int32_t height,
                     int64_t frame, const std::string& propsJson,
                     MotionFrame& out, std::string& chosen, std::string& why,
                     bool wrap = false) {
    std::vector<MotionTemplateInfo> templates;
    if (!MotionTemplateCatalog(templates, why)) {
        return false;
    }
    const MotionTemplateInfo* piece = Find(templates, name);
    if (piece == nullptr) {
        why = "no shipped piece '" + name + "'";
        return false;
    }
    Value instance;
    Value props;
    MotionComposition composition;
    MotionError motion;
    FakeMeasurer fixed;
    WrappingMeasurer wrapping;
    MotionMeasurer& measurer =
        wrap ? static_cast<MotionMeasurer&>(wrapping) : fixed;
    if (!InstantiateMotionTemplate(piece->definition, width, height, instance,
                                   chosen, why) ||
        (!propsJson.empty() && !Value::Parse(propsJson, props, why))) {
        return false;
    }
    if (!ParseMotionComposition(instance, composition, motion) ||
        !EvaluateMotionFrame(composition, props, frame, measurer, out,
                             motion)) {
        why = motion.message;
        return false;
    }
    return true;
}

MotionSafeAreaReport CheckIn(const MotionFrame& frame,
                             const MotionSafeZone& zone) {
    MotionSafeAreaReport report;
    report.zone = zone;
    CheckMotionFrameSafeArea(frame, report);
    return report;
}

const MotionEvaluatedNode* FindNode(const MotionEvaluatedNode& node,
                                    const std::string& id) {
    if (node.id == id) {
        return &node;
    }
    for (const MotionEvaluatedNode& child : node.children) {
        if (const MotionEvaluatedNode* found = FindNode(child, id)) {
            return found;
        }
    }
    return nullptr;
}

// M5b -- the logo piece in 9:16 keeps the social apps' margins in every
// corner (the author, 6 November 2026: "picto hors safe zone"); at the old 6 %
// it left the social zone by the top and the right. 16:9 and 1:1 keep 5 %.
void CheckLogoSocialZone() {
    const MotionSafeZone social = SocialVerticalMotionSafeZone();
    const MotionSafeZone standard = DefaultMotionSafeZone();
    for (const char* corner :
         {"haut-droite", "haut-gauche", "bas-droite", "bas-gauche"}) {
        MotionFrame frame;
        std::string chosen;
        std::string why;
        if (!EvaluateShipped("logo", 1080, 1920, 100,
                             std::string(R"({"coin":")") + corner + "\"}",
                             frame, chosen, why)) {
            Check(false, std::string("the logo evaluates in 9x16 at ") +
                             corner + ": " + why);
            continue;
        }
        const MotionSafeAreaReport inSocial = CheckIn(frame, social);
        const MotionSafeAreaReport inDefault = CheckIn(frame, standard);
        // On the left the logo starts where the 9:16 name card does (9 %,
        // 97 px); on the right it ends before the button rail (14 %).
        if (const MotionEvaluatedNode* image = FindNode(frame.root, "logo")) {
            const bool left =
                std::string(corner).find("gauche") != std::string::npos;
            Check(left
                      ? std::abs(image->box.x - 97.2) < 0.6
                      : std::abs(image->box.x + image->box.width - 928.8) < 0.6,
                  std::string("the 9x16 logo at ") + corner +
                      " keeps its side margin: x " +
                      std::to_string(image->box.x) + ", width " +
                      std::to_string(image->box.width));
        } else {
            Check(false,
                  std::string("the logo node is evaluated at ") + corner);
        }
        Check(chosen == "9x16" && inSocial.issues.empty() &&
                  inDefault.issues.empty(),
              std::string("the shipped logo in 9x16, corner ") + corner +
                  ", is inside both zones: social " +
                  SerializeMotionSafeAreaReport(inSocial) + " / default " +
                  SerializeMotionSafeAreaReport(inDefault));
    }
    // The margins of 6 October: 6 % on every side, which the 6 November
    // planche showed outside the social zone, top and right.
    MotionFrame old;
    std::string chosen;
    std::string why;
    if (EvaluateShipped(
            "logo", 1080, 1920, 100,
            R"({"marge_gauche":6,"marge_droite":6,"marge_haut":6,"marge_bas":6})",
            old, chosen, why)) {
        const MotionSafeAreaReport report = CheckIn(old, social);
        Check(report.issues.size() == 1 &&
                  report.issues[0].kind == "outside_safe_area" &&
                  report.issues[0].node_id == "logo" &&
                  report.issues[0].edges ==
                      std::vector<std::string>{"right", "top"},
              "the logo at 6 % leaves the social zone by the top and the "
              "right, the 6 November case: " +
                  SerializeMotionSafeAreaReport(report));
        Check(CheckIn(old, standard).issues.empty(),
              "the logo at 6 % still passes the 5 % zone");
    } else {
        Check(false, "the logo evaluates at the old margins: " + why);
    }
    for (const auto& [width, height] :
         std::vector<std::pair<int32_t, int32_t>>{{1920, 1080}, {1080, 1080}}) {
        MotionFrame frame;
        if (!EvaluateShipped("logo", width, height, 100, "", frame, chosen,
                             why)) {
            Check(false, "the logo evaluates in " + chosen + ": " + why);
            continue;
        }
        Check(CheckIn(frame, standard).issues.empty(),
              "the shipped logo in " + chosen + " is inside the 5 % zone");
    }
}

// M5b -- the marquee: its middle occurrence is the text read, named and
// judged; the echoes are decor, cut by the frame on purpose and listed,
// never judged (the author, 6 November 2026: "il sort de l'écran").
void CheckBandeauCentre() {
    const MotionSafeZone social = SocialVerticalMotionSafeZone();
    const MotionSafeZone standard = DefaultMotionSafeZone();
    // Locked (frame 40), at the shipped defaults, in each format.
    for (const auto& [width, height, zone] :
         std::vector<std::tuple<int32_t, int32_t, MotionSafeZone>>{
             {1080, 1920, social},
             {1920, 1080, standard},
             {1080, 1080, standard}}) {
        MotionFrame frame;
        std::string chosen;
        std::string why;
        if (!EvaluateShipped("bandeau", width, height, 40, "", frame, chosen,
                             why)) {
            Check(false, "the marquee evaluates in " + chosen + ": " + why);
            continue;
        }
        const MotionSafeAreaReport report = CheckIn(frame, zone);
        const bool centresJudged =
            std::find(report.decor.begin(), report.decor.end(),
                      "haut-centre") == report.decor.end() &&
            std::find(report.decor.begin(), report.decor.end(), "bas-centre") ==
                report.decor.end();
        Check(
            report.issues.empty() && report.decor.size() == 16 && centresJudged,
            "the shipped marquee in " + chosen +
                " passes at rest, its 16 echoes left out and said: " +
                SerializeMotionSafeAreaReport(report));
    }
    // A phrase of 24 characters fits the 9:16 social zone at the shipped
    // size on one line: the FakeMeasurer gives half an em per byte, 24 x 32
    // = 768 px, within the 800 px a text centred in the frame has between
    // the zone's left (65) and TikTok's rail (940), the 9:16 measure.
    MotionFrame fits;
    std::string chosen;
    std::string why;
    if (!EvaluateShipped("bandeau", 1080, 1920, 40,
                         R"({"haut":"le festival des lumieres",)"
                         R"("bas":"ouvert tout l'ete"})",
                         fits, chosen, why, true)) {
        Check(false, "the marquee evaluates with its phrases: " + why);
        return;
    }
    const MotionSafeAreaReport fitting = CheckIn(fits, social);
    const MotionEvaluatedNode* oneLine = FindNode(fits.root, "haut-centre");
    Check(fitting.issues.empty() && oneLine != nullptr &&
              oneLine->text_layout.lines.size() == 1,
          "a phrase of 24 characters fits the 9:16 social zone on one line: " +
              SerializeMotionSafeAreaReport(fitting));
    // A longer one wraps at the measure instead of leaving the frame, in
    // each format: every line of the read occurrence within the measure,
    // nothing outside the zone.
    const std::string longer =
        R"({"haut":"une phrase beaucoup trop longue pour le cadre",)"
        R"("bas":"et sa reponse qui ne tient pas non plus"})";
    for (const auto& [width, height, zone, measure] :
         std::vector<std::tuple<int32_t, int32_t, MotionSafeZone, double>>{
             {1080, 1920, social, 800.0},
             {1920, 1080, standard, 1728.0},
             {1080, 1080, standard, 972.0}}) {
        MotionFrame wrapped;
        if (!EvaluateShipped("bandeau", width, height, 40, longer, wrapped,
                             chosen, why, true)) {
            Check(false, "the marquee evaluates in " + chosen + ": " + why);
            continue;
        }
        const MotionSafeAreaReport report = CheckIn(wrapped, zone);
        bool within = true;
        size_t lines = 0;
        for (const char* id : {"haut-centre", "bas-centre"}) {
            const MotionEvaluatedNode* read = FindNode(wrapped.root, id);
            if (read == nullptr) {
                within = false;
                continue;
            }
            lines = std::max(lines, read->text_layout.lines.size());
            for (const MotionTextLine& line : read->text_layout.lines) {
                within = within && line.width <= measure + 1e-6;
            }
        }
        Check(report.issues.empty() && within && lines == 2,
              "in " + chosen + " a long phrase wraps onto two lines within " +
                  std::to_string(static_cast<int>(measure)) +
                  " px and stays in the zone: " +
                  SerializeMotionSafeAreaReport(report));
    }
    // The measure undone, the same phrase leaves the zone, and only the
    // middle occurrence says so.
    MotionFrame wide;
    if (!EvaluateShipped("bandeau", 1080, 1920, 40,
                         R"({"haut":"une phrase beaucoup trop longue pour)"
                         R"( le cadre","mesure":4000})",
                         wide, chosen, why, true)) {
        Check(false, "the marquee evaluates without its measure: " + why);
        return;
    }
    const MotionSafeAreaReport overflowing = CheckIn(wide, social);
    Check(overflowing.issues.size() == 1 &&
              overflowing.issues[0].kind == "outside_safe_area" &&
              overflowing.issues[0].node_id == "haut-centre" &&
              overflowing.issues[0].edges ==
                  std::vector<std::string>{"left", "right"} &&
              overflowing.decor.size() == 16,
          "without its measure a phrase too wide is refused on haut-centre "
          "alone, the echoes still left out: " +
              SerializeMotionSafeAreaReport(overflowing));
    const std::string json = SerializeMotionSafeAreaReport(overflowing);
    Check(json.find("\"decor\":[\"") != std::string::npos &&
              json.find("haut-centre") != std::string::npos,
          "the report lists the decor it left out: " + json);
}

// M5b -- `decor` in the check and in the parser: a decor node and its
// descendants are skipped and listed; the field is a boolean.
void CheckDecor() {
    MotionEvaluatedNode a = TextNode("a", 900, 1000, 400, {"Camille"});
    a.decor = true;
    MotionSafeAreaReport report = CheckOne(Frame({a}));
    Check(
        report.issues.empty() && report.decor == std::vector<std::string>{"a"},
        "a decor text past the edge is not an issue, and is listed");
    MotionEvaluatedNode parent;
    parent.id = "mur";
    parent.type = MotionNodeType::Box;
    parent.decor = true;
    parent.box = {0, 0, 1080, 1920};
    parent.children.push_back(TextNode("b", 900, 1000, 400, {"Camille"}));
    report = CheckOne(Frame({parent}));
    Check(report.issues.empty() &&
              report.decor == std::vector<std::string>{"mur"},
          "a decor box takes its children with it, listed once by its id");
    MotionEvaluatedNode unnamed = TextNode("", 900, 1000, 400, {"Camille"});
    unnamed.uid.clear();
    unnamed.path = "root.children[0]";
    unnamed.decor = true;
    report =
        CheckOne(Frame({unnamed, TextNode("c", 900, 1000, 400, {"Camille"})}));
    Check(report.issues.size() == 1 && report.issues[0].node_id == "c" &&
              report.decor == std::vector<std::string>{"root.children[0]"},
          "a decor node without id is listed by its path, its sibling judged");
    Check(SerializeMotionSafeAreaReport(report).find(
              R"("decor":["root.children[0]"])") != std::string::npos,
          "the serialized report carries the decor list");
    Check(
        SerializeMotionSafeAreaReport(CheckOne(Frame({}))).find("\"decor\"") ==
            std::string::npos,
        "no decor, no list");

    // An element wholly off the frame is not seen: the second band of the
    // marquee waits beyond the right edge before it enters. One that still
    // shows a sliver is.
    MotionSafeAreaReport waiting;
    CheckMotionFrameSafeArea(
        Frame({TextNode("d", 1200, 1000, 400, {"Camille"})}, 0), waiting);
    Check(waiting.issues.empty() && waiting.in_motion == 0,
          "a text wholly past the right edge is neither an issue nor a "
          "passage");
    CheckMotionFrameSafeArea(
        Frame({TextNode("d", 1070, 1000, 400, {"Camille"})}, 1), waiting);
    Check(waiting.issues.size() == 1 && waiting.issues[0].node_id == "d" &&
              !waiting.issues[0].at_rest,
          "entering from off-frame, it is judged, and in motion");
    Check(CheckOne(Frame({TextNode("e", 0, 1920, 400, {"Camille"})}))
              .issues.empty(),
          "a text wholly under the bottom edge is not seen");

    // What an overflow: hidden ancestor hides is not seen; what it leaves
    // is judged as the visible part. A box without overflow hides nothing.
    const auto clipped = [](double boxX, double boxWidth, bool overflow) {
        MotionEvaluatedNode box;
        box.id = "fenetre";
        box.type = MotionNodeType::Box;
        box.overflow_hidden = overflow;
        box.box = {boxX, 900, boxWidth, 200};
        box.children.push_back(TextNode("f", 900, 1000, 400, {"Camille"}));
        return CheckOne(Frame({box}));
    };
    Check(clipped(0, 900, true).issues.empty(),
          "a text wholly hidden by its window is not seen");
    Check(clipped(0, 1000, true).issues.empty(),
          "a text whose visible part stays inside the zone passes");
    MotionSafeAreaReport sliver = clipped(1000, 200, true);
    Check(sliver.issues.size() == 1 && sliver.issues[0].node_id == "f" &&
              sliver.issues[0].bounds.x == 1000 &&
              sliver.issues[0].bounds.width == 40 &&
              sliver.issues[0].edges == std::vector<std::string>{"right"},
          "a text is judged on what its window leaves of it: " +
              SerializeMotionSafeAreaReport(sliver));
    Check(clipped(0, 900, false).issues.size() == 1,
          "a box without overflow hides nothing");

    MotionComposition composition;
    MotionError error;
    Value definition;
    std::string parseError;
    Value::Parse(R"({"width":100,"height":100,"fps":25,"duration":10,)"
                 R"("root":{"type":"fill","children":[)"
                 R"({"type":"box","decor":"yes"}]}})",
                 definition, parseError);
    Check(
        !ParseMotionComposition(definition, composition, error) &&
            error.message.find("decor must be a boolean") != std::string::npos,
        "decor is a boolean: " + error.message);
    Value::Parse(R"({"width":100,"height":100,"fps":25,"duration":10,)"
                 R"("root":{"type":"fill","children":[)"
                 R"({"type":"box","decor":true,"children":[)"
                 R"({"type":"text","content":"x"}]}]}})",
                 definition, parseError);
    Check(ParseMotionComposition(definition, composition, error) &&
              composition.root.children.size() == 1 &&
              composition.root.children[0].decor &&
              !composition.root.children[0].children[0].decor,
          "decor is read on the node that says it: " + error.message);
}

// M5b -- the charter's `pieces`: by piece, the values of its own props (the
// second line of `nom` in colour on white, the logo file of `logo`), checked
// against the piece when it is added, and counted among the colours and
// fonts the charter allows.
void CheckCharterPieces() {
    MotionKitInfo kit;
    std::string error;
    const auto refused = [&](const std::string& json,
                             const std::string& reason) {
        return !ParseMotionKit("k", json, kit, error) &&
               error.find(reason) != std::string::npos;
    };
    const std::string brand =
        R"({"format":"brand-kit.v1","description":"Charte.",)";
    Check(refused(R"({"format":"motion-kit.v1","description":"x",)"
                  R"("props":{"accent":"#000000"},)"
                  R"("pieces":{"nom":{"texte_fonction":"#0A7C66"}}})",
                  "unknown field 'pieces': a field of a brand's charter"),
          "pieces belong to a charter, not a motion kit: " + error);
    Check(refused(brand + R"("pieces":[]})", "'pieces' must be an object"),
          "pieces is an object by piece name: " + error);
    Check(refused(brand + R"("pieces":{"deux mots":{"a":1}}})",
                  "'pieces.deux mots' is not a piece name"),
          "a piece name is a file name: " + error);
    Check(refused(brand + R"("pieces":{"nom":{}}})",
                  "'pieces.nom' must be an object of prop values, at least "
                  "one"),
          "a piece section sets something: " + error);
    Check(refused(brand + R"("pieces":{"nom":{"corps":[64]}}})",
                  "'pieces.nom.corps' must be a string, a number or a "
                  "boolean"),
          "a value is a literal a prop takes: " + error);
    Check(refused(brand + R"("pieces":{"nom":{"fonction":""}}})",
                  "'pieces.nom.fonction' is empty"),
          "an empty text sets nothing: " + error);
    Check(refused(brand + R"("pieces":{"nom":{"texte":"bleu roi"}}})",
                  "'pieces.nom.texte' must be a color the motion format "
                  "reads (#rrggbb...), as the shared prop 'texte' is"),
          "a shared role keeps its type inside pieces: " + error);
    Check(refused(brand + R"("props":{},"pieces":{"nom":{"corps":64}}})",
                  "'props' must be an object that sets some"),
          "an empty props object is still refused: " + error);

    const std::string withPieces =
        brand +
        R"("props":{"accent":"#0A7C66","texte":"#FFFFFF","police":"Exemple Sans"},)"
        R"("pieces":{)"
        R"("nom":{"fond_fonction":"#FFFFFF","texte_fonction":"#0A7C66"},)"
        R"("logo":{"logo":"/kits/marque/logo-compact.png","marge_droite":10,)"
        R"("coin":"haut-gauche"}}})";
    Check(ParseMotionKit("marque", withPieces, kit, error) &&
              kit.pieces.IsObject() && kit.pieces.Find("nom") != nullptr,
          "a charter with pieces reads: " + error);
    Check(
        MotionKitColors(kit) == std::vector<std::string>{"#0a7c66", "#ffffff"},
        "the pieces' colours count among the charter's, a path does not");
    Check(ParseMotionKit("seul", brand + R"("pieces":{"nom":{"corps":64}}})",
                         kit, error),
          "a charter of pieces alone sets something: " + error);
    Check(refused(brand + R"("palette":[]})", "a charter sets something"),
          "a charter that sets nothing is still refused: " + error);
    Check(ParseMotionKit("marque", withPieces, kit, error),
          "the charter with pieces again: " + error);

    std::vector<MotionTemplateInfo> shipped;
    Check(MotionTemplateCatalog(shipped, error), "catalog loads: " + error);
    const MotionTemplateInfo* nom = Find(shipped, "nom");
    const MotionTemplateInfo* logo = Find(shipped, "logo");
    const MotionTemplateInfo* carton = Find(shipped, "carton");
    if (nom == nullptr || logo == nullptr || carton == nullptr) {
        Check(false, "nom, logo and carton are shipped");
        return;
    }
    Check(DefaultOf(nom->definition, "fond_fonction") == "#000000" &&
              DefaultOf(nom->definition, "texte_fonction") == "#FFFFFF",
          "the shipped nom keeps its second line white on black");
    MotionKitApplication application;
    Value dressed = nom->definition;
    Check(ApplyMotionKit(kit, "nom", dressed, application, error) &&
              application.applied ==
                  std::vector<std::string>{"accent", "texte", "police",
                                           "fond_fonction", "texte_fonction"} &&
              application.not_in_piece.empty() &&
              DefaultOf(dressed, "fond_fonction") == "#FFFFFF" &&
              DefaultOf(dressed, "texte_fonction") == "#0A7C66" &&
              DefaultOf(dressed, "accent") == "#0A7C66",
          "nom takes the charter's roles, then its own second line: " + error);
    Value instance;
    std::string format;
    Check(InstantiateMotionTemplate(dressed, 1080, 1920, instance, format,
                                    error) &&
              DefaultOf(instance, "texte_fonction") == "#0A7C66" &&
              DefaultOf(instance, "fond_fonction") == "#FFFFFF",
          "the 9:16 instance keeps the second line green on white: " + error);
    Value badge = logo->definition;
    Check(ApplyMotionKit(kit, "logo", badge, application, error) &&
              application.applied ==
                  std::vector<std::string>{"logo", "marge_droite", "coin"} &&
              application.not_in_piece ==
                  std::vector<std::string>{"accent", "texte", "police"} &&
              DefaultOf(badge, "logo") == "/kits/marque/logo-compact.png" &&
              DefaultOf(badge, "coin") == "haut-gauche",
          "logo takes its file and its corner from the charter: " + error);
    // The charter holds in each format: the 9:16 format of logo set
    // marge_droite 14, the charter's 10 replaces it there too.
    Check(
        InstantiateMotionTemplate(badge, 1080, 1920, instance, format, error) &&
            instance.Dump().find(
                R"("marge_droite":{"type":"number","unit":"%",)"
                R"("min":0,"max":40,"default":10})") != std::string::npos,
        "a format that set the prop takes the charter's value: " +
            instance.Dump());
    Value card = carton->definition;
    Check(ApplyMotionKit(kit, "carton", card, application, error) &&
              std::find(application.applied.begin(), application.applied.end(),
                        "fond_fonction") == application.applied.end(),
          "a piece the charter names nothing for takes the roles alone: " +
              error);

    // Refusals at application, by name.
    const auto rejects = [&](const std::string& pieces,
                             const std::string& piece,
                             const std::string& reason) {
        MotionKitInfo other;
        std::string why;
        if (!ParseMotionKit("k", brand + R"("pieces":)" + pieces + "}", other,
                            why)) {
            Check(false, "the charter reads: " + why);
            return false;
        }
        const MotionTemplateInfo* target = Find(shipped, piece);
        Value definition = target->definition;
        MotionKitApplication app;
        const bool refusedNow =
            !ApplyMotionKit(other, piece, definition, app, why) &&
            why.find(reason) != std::string::npos;
        if (!refusedNow) {
            std::cerr << "  got: " << why << '\n';
        }
        return refusedNow;
    };
    Check(rejects(R"({"nom":{"couleur":"#000000"}})", "nom",
                  "the piece 'nom' has no prop 'couleur'"),
          "a prop the piece lacks is refused by name");
    Check(rejects(R"({"nom":{"corps":999}})", "nom",
                  "the prop takes a number between 16 and 200"),
          "a number out of its bounds is refused");
    Check(rejects(R"({"nom":{"corps":"grand"}})", "nom",
                  "the prop takes a number"),
          "a text for a number is refused");
    Check(rejects(R"({"logo":{"coin":"milieu"}})", "logo",
                  "the prop takes one of \"haut-droite\", \"haut-gauche\""),
          "a word outside an enum is refused");
    Check(rejects(R"({"nom":{"fonction":12}})", "nom",
                  "the prop takes a non-empty text"),
          "a number for a text is refused");
    Check(rejects(R"({"nom":{"texte_group":"x"}})", "nom",
                  "'group' is none of these"),
          "a group has no value to set");

    // A piece's own value wins over a shared role, and is said once.
    MotionKitInfo specific;
    Check(ParseMotionKit("k",
                         brand + R"("props":{"texte":"#FFFFFF"},)"
                                 R"("pieces":{"nom":{"texte":"#111111"}}})",
                         specific, error),
          "a charter whose piece section repeats a role: " + error);
    Value twice = nom->definition;
    Check(ApplyMotionKit(specific, "nom", twice, application, error) &&
              DefaultOf(twice, "texte") == "#111111" &&
              application.applied == std::vector<std::string>{"texte"},
          "the piece's own value wins over the role, listed once: " + error);
    Check(MotionKitFonts(MotionKitInfo{}).empty(), "no fonts from nothing");
    MotionKitInfo fonted;
    Check(
        ParseMotionKit("k",
                       brand + R"("pieces":{"nom":{"police":"Exemple Sans"}}})",
                       fonted, error) &&
            MotionKitFonts(fonted) == std::vector<std::string>{"Exemple Sans"},
        "a piece's police counts among the charter's fonts: " + error);

    // M5b -- a charter withdraws a piece, with the reason (the author on a
    // brand's charter, 6 November 2026: carton "pas besoin", maillage
    // withdrawn).
    Check(
        refused(brand + R"("withdrawn":[]})", "'withdrawn' must be an object"),
        "withdrawn is an object: " + error);
    Check(
        refused(brand + R"("withdrawn":{}})", "'withdrawn' must be an object"),
        "an empty withdrawn is refused: " + error);
    Check(refused(brand + R"("withdrawn":{"../carton":"non"}})",
                  "'withdrawn.../carton' is not a piece name"),
          "a withdrawn name is a piece name: " + error);
    Check(refused(brand + R"("withdrawn":{"carton":""}})",
                  "'withdrawn.carton' must say why"),
          "a withdrawal says why: " + error);
    Check(refused(brand + R"("withdrawn":{"carton":true}})",
                  "'withdrawn.carton' must say why"),
          "a reason is a text: " + error);
    Check(refused(brand + R"("pieces":{"nom":{"corps":64}},)"
                          R"("withdrawn":{"nom":"non"}})",
                  "the piece 'nom' is both withdrawn and given values"),
          "a piece is not both withdrawn and dressed: " + error);
    MotionKitInfo withdrawing;
    Check(ParseMotionKit("seul", brand + R"("withdrawn":{"carton":"non"}})",
                         withdrawing, error),
          "a charter that only withdraws sets something: " + error);
    Check(ParseMotionKit(
              "marque",
              brand + R"("props":{"accent":"#0A7C66"},)"
                      R"("withdrawn":{"carton":"« pas besoin », l'auteur, )"
                      R"(6 novembre 2026","maillage":"retiré"}})",
              withdrawing, error) &&
              MotionKitWithdrawal(withdrawing, "carton") ==
                  "« pas besoin », l'auteur, 6 novembre 2026" &&
              MotionKitWithdrawal(withdrawing, "nom").empty(),
          "the reason is read back by piece: " + error);
    Value kept = carton->definition;
    const std::string before = kept.Dump();
    Check(!ApplyMotionKit(withdrawing, "carton", kept, application, error) &&
              error.find("kit 'marque' does not offer the piece 'carton': "
                         "« pas besoin »") != std::string::npos &&
              kept.Dump() == before,
          "a withdrawn piece is refused with the reason, left as it was: " +
              error);
    Value offered = nom->definition;
    Check(ApplyMotionKit(withdrawing, "nom", offered, application, error) &&
              DefaultOf(offered, "accent") == "#0A7C66",
          "the pieces it does not withdraw are dressed as before: " + error);
}

// DEC62/CH2 -- docs/roadmap/CHARTE-2026-11.md. A value for one format of a
// piece: read for its shape, applied to that format alone over the value
// for every format, refused by name when the piece has no such format, no
// such prop or does not take the value, and counted in what the charter
// allows.
std::string DefaultLiteral(const Value& definition, const std::string& prop) {
    const Value* props = definition.Find("props");
    const Value* entry = props != nullptr ? props->Find(prop) : nullptr;
    const Value* value = entry != nullptr ? entry->Find("default") : nullptr;
    return value != nullptr ? value->Dump() : "";
}

void CheckCharterPieceFormats() {
    MotionKitInfo kit;
    std::string error;
    const std::string brand =
        R"({"format":"brand-kit.v1","description":"Charte.",)";
    const auto refused = [&](const std::string& pieces,
                             const std::string& reason) {
        return !ParseMotionKit("k", brand + R"("pieces":)" + pieces + "}", kit,
                               error) &&
               error.find(reason) != std::string::npos;
    };
    Check(refused(R"({"logo":{"formats":[]}})",
                  "'pieces.logo.formats' must be an object"),
          "formats is an object by format name: " + error);
    Check(refused(R"({"logo":{"formats":{}}})",
                  "'pieces.logo.formats' must be an object"),
          "an empty formats is refused: " + error);
    Check(refused(R"({"logo":{"formats":{"9 16":{"marge_haut":20}}}})",
                  "'pieces.logo.formats.9 16' is not a format name"),
          "a format name is a name: " + error);
    Check(refused(R"({"logo":{"formats":{"9x16":{}}}})",
                  "'pieces.logo.formats.9x16' must be an object of prop "
                  "values, at least one"),
          "a format sets something: " + error);
    Check(refused(R"({"logo":{"formats":{"9x16":"haut"}}})",
                  "'pieces.logo.formats.9x16' must be an object of prop "
                  "values"),
          "a format is an object of values: " + error);
    Check(refused(R"({"logo":{"formats":{"9x16":{"marge_haut":[20]}}}})",
                  "'pieces.logo.formats.9x16.marge_haut' must be a string, a "
                  "number or a boolean"),
          "a per-format value is a literal: " + error);
    Check(refused(R"({"logo":{"formats":{"9x16":{"logo":""}}}})",
                  "'pieces.logo.formats.9x16.logo' is empty"),
          "an empty per-format text: " + error);
    Check(refused(R"({"nom":{"formats":{"9x16":{"texte":"bleu roi"}}}})",
                  "'pieces.nom.formats.9x16.texte' must be a color the motion "
                  "format reads (#rrggbb...), as the shared prop 'texte' is"),
          "a shared role keeps its type in a format: " + error);
    Check(refused(R"({"logo":{"marge_haut":{"9x16":20}}})",
                  "'pieces.logo.marge_haut' must be a string, a number or a "
                  "boolean"),
          "only `formats` holds an object: " + error);

    std::vector<MotionTemplateInfo> shipped;
    Check(MotionTemplateCatalog(shipped, error), "catalog loads: " + error);
    const MotionTemplateInfo* logo = Find(shipped, "logo");
    const MotionTemplateInfo* carton = Find(shipped, "carton");
    if (logo == nullptr || carton == nullptr) {
        Check(false, "logo and carton are shipped");
        return;
    }
    // A margin for every format, and another for 9x16: 9x16 takes its own,
    // the others the one for every format.
    Check(ParseMotionKit("marque",
                         brand + R"("pieces":{"logo":{"marge_haut":10,)"
                                 R"("formats":{"9x16":{"marge_haut":20}}}}})",
                         kit, error),
          "a charter with a value for one format reads: " + error);
    MotionKitApplication application;
    Value badge = logo->definition;
    Check(ApplyMotionKit(kit, "logo", badge, application, error) &&
              application.applied == std::vector<std::string>{"marge_haut"} &&
              DefaultLiteral(badge, "marge_haut") == "10",
          "the value for every format is the default, the prop listed once: " +
              error);
    const auto marginIn = [&](const Value& definition, int32_t width,
                              int32_t height, const std::string& prop) {
        Value instance;
        std::string format;
        std::string why;
        return InstantiateMotionTemplate(definition, width, height, instance,
                                         format, why)
                   ? DefaultLiteral(instance, prop)
                   : "refused: " + why;
    };
    Check(marginIn(badge, 1080, 1920, "marge_haut") == "20",
          "in 9:16 the logo takes the charter's 9x16 margin: " +
              marginIn(badge, 1080, 1920, "marge_haut"));
    Check(marginIn(badge, 1920, 1080, "marge_haut") == "10" &&
              marginIn(badge, 1080, 1080, "marge_haut") == "10",
          "in 16:9 and 1:1 the margin for every format: " +
              marginIn(badge, 1920, 1080, "marge_haut"));
    Check(marginIn(badge, 1080, 1920, "marge_gauche") == "9" &&
              marginIn(badge, 1920, 1080, "largeur") == "10",
          "what the charter does not name stays the piece's, per format");

    const auto rejects = [&](const std::string& pieces,
                             const std::string& piece,
                             const std::string& reason) {
        MotionKitInfo other;
        std::string why;
        if (!ParseMotionKit("k", brand + R"("pieces":)" + pieces + "}", other,
                            why)) {
            std::cerr << "  does not read: " << why << '\n';
            return false;
        }
        Value definition = Find(shipped, piece)->definition;
        const std::string before = definition.Dump();
        MotionKitApplication app;
        const bool refusedNow =
            !ApplyMotionKit(other, piece, definition, app, why) &&
            why.find(reason) != std::string::npos &&
            definition.Dump() == before;
        if (!refusedNow) {
            std::cerr << "  got: " << why << '\n';
        }
        return refusedNow;
    };
    Check(rejects(R"({"logo":{"formats":{"4x5":{"marge_haut":20}}}})", "logo",
                  "kit 'k' sets 'pieces.logo.formats.4x5', and the piece "
                  "'logo' has no format '4x5'; it has 9x16, 16x9, 1x1"),
          "a format the piece does not declare is refused, with its formats");
    Check(rejects(R"({"logo":{"formats":{"9x16":{"couleur":"#000000"}}}})",
                  "logo",
                  "'pieces.logo.formats.9x16.couleur', and the piece 'logo' "
                  "has no prop 'couleur'"),
          "a prop the piece lacks, in a format, is refused by name");
    Check(rejects(R"({"logo":{"formats":{"9x16":{"marge_haut":99}}}})", "logo",
                  "gives 'pieces.logo.formats.9x16.marge_haut' 99, and the "
                  "prop takes a number between 0 and 60"),
          "a per-format value out of its bounds is refused");

    // A charter that gives a piece values for one format only applies, and
    // gives a format without props its own.
    Check(ParseMotionKit("marque",
                         brand + R"("pieces":{"carton":{"formats":)"
                                 R"({"9x16":{"corps":120}}}}})",
                         kit, error),
          "a charter of per-format values alone reads: " + error);
    Value card = carton->definition;
    Check(ApplyMotionKit(kit, "carton", card, application, error) &&
              application.applied == std::vector<std::string>{"corps"} &&
              DefaultLiteral(card, "corps") == "96",
          "per-format values alone apply, the default untouched: " + error);
    const Value* nineBySixteen = FindMotionKitPath(card, "formats.9x16.props");
    Check(
        nineBySixteen != nullptr && nineBySixteen->Dump() == R"({"corps":120})",
        "the 9x16 format, which had no props, gets its own");
    Check(marginIn(card, 1080, 1920, "corps") == "120" &&
              marginIn(card, 1920, 1080, "corps") == "110" &&
              marginIn(card, 1080, 1080, "corps") == "88",
          "only 9:16 changes; 16:9 and 1:1 keep their own size: " +
              marginIn(card, 1080, 1920, "corps"));

    // The colours and fonts of a format count among the charter's.
    Check(ParseMotionKit("marque",
                         brand +
                             R"("pieces":{"nom":{"texte_fonction":"#0A7C66",)"
                             R"("formats":{"9x16":{"fond_fonction":"#E0A030",)"
                             R"("police":"Exemple Sans"},)"
                             R"("16x9":{"police":"Exemple Sans Mono"}}}}})",
                         kit, error),
          "a charter with per-format colours and fonts reads: " + error);
    Check(
        MotionKitColors(kit) == std::vector<std::string>{"#0a7c66", "#e0a030"},
        "a per-format colour is allowed");
    Check(MotionKitFonts(kit) ==
              std::vector<std::string>{"Exemple Sans", "Exemple Sans Mono"},
          "a per-format font is allowed");
}

int main() {
    CheckCatalog();
    CheckHighlighterPiece();
    CheckPieceFile();
    CheckLibrary();
    CheckKits();
    CheckBrandKits();
    CheckLibraryBase();
    CheckPlacement();
    CheckSafeArea();
    CheckSafeAreaAtRest();
    CheckSocialZone();
    CheckTimelineZone();
    CheckShelfSearch();
    CheckLogoSocialZone();
    CheckBandeauCentre();
    CheckDecor();
    CheckCharterPieces();
    CheckCharterPieceFormats();
    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "motion templates tests passed\n";
    return 0;
}
