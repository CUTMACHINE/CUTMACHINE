#include "ColorManagement.h"
#include "ColorMeasure.h"
#include "Document.h"
#include "Timeline.h"
#include "Ulid.h"
#include "VideoScopes.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>

#include "TestSupport.h"

namespace {

Document ValidDocument() {
    Document document;
    document.sources = {
        {"01K00000000000000000000001", "A.MP4", {25, 1}, {200, 25}},
        {"01K00000000000000000000002", "B.MP4", {25, 1}, {200, 25}},
    };
    document.sequence.tracks = {
        {"01K00000000000000000000003",
         "video",
         0,
         {
             {"01K00000000000000000000004",
              "01K00000000000000000000001",
              {100, 25},
              {2, 25},
              {0, 25}},
             {"01K00000000000000000000005",
              "01K00000000000000000000002",
              {10, 25},
              {2, 25},
              {2, 25}},
         }},
        {"01K00000000000000000000006",
         "video",
         1,
         {
             {"01K00000000000000000000007",
              "01K00000000000000000000001",
              {0, 25},
              {1, 25},
              {1, 25}},
         }},
    };
    return document;
}

void ExpectInvalid(Document document, const std::string& expected,
                   const std::string& label) {
    std::string error;
    Document loaded;
    Check(!Document::LoadFromString(document.SaveToString(), loaded, error),
          label + " must be rejected while loading");
    Check(
        error.find(expected) != std::string::npos,
        label + " error must contain '" + expected + "', got '" + error + "'");
}

}  // namespace

int main() {
    Test("reference color transfer functions", [] {
        const auto Near = [](double actual, double expected, double epsilon,
                             const std::string& message) {
            Check(std::abs(actual - expected) <= epsilon,
                  message + ": got " + std::to_string(actual));
        };
        Near(DecodeSonySLog3(95.0 / 1023.0), 0.0, 1e-12,
             "S-Log3 code 95 is scene black");
        Near(DecodeSonySLog3(420.0 / 1023.0), 0.18, 1e-12,
             "S-Log3 code 420 is 18% grey");
        Near(DecodeSonySLog3(598.0 / 1023.0), 0.9, 0.005,
             "S-Log3 code 598 is 90% white");
        for (double value : {-0.01, 0.0, 0.0078125, 0.18, 4.0})
            Near(DecodeAcesCct(EncodeAcesCct(value)), value, 1e-12,
                 "ACEScct round trip");
        Near(EncodeHlg(DecodeHlg(0.75)), 0.75, 1e-9,
             "HLG reference white signal round trip");
        Near(EncodeHlg(0.9 * HlgSceneReflectionScale()), 0.75, 1e-9,
             "90% scene white maps to HLG reference white");
        ColorManagementSettings sony;
        sony.enabled = true;
        sony.input_gamut = "sony_sgamut3_cine";
        sony.input_transfer = "sony_slog3";
        sony.input_ycbcr_matrix = "bt709";
        sony.input_range = "full";
        sony.working_gamut = "acescct";
        sony.output_gamut = "rec2020";
        sony.output_transfer = "hlg";
        const double whiteSignal = 598.0 / 1023.0;
        const RgbColor hlg = TransformColorForOutput(
            sony, {whiteSignal, whiteSignal, whiteSignal});
        Near(hlg.red, 0.75, 0.005,
             "neutral S-Log3 90% white maps to HLG reference white");
        Near(hlg.red, hlg.green, 2e-5,
             "neutral conversion remains neutral in Rec.2020");
        Near(hlg.green, hlg.blue, 2e-5, "neutral conversion has no blue cast");
        // ITW5 -- a BT.2100 HLG rush in, HLG out: reference white stays
        // reference white, and the OCIO path builds for it.
        ColorManagementSettings hlgRush = sony;
        hlgRush.input_gamut = "rec2020";
        hlgRush.input_transfer = "hlg";
        hlgRush.input_ycbcr_matrix = "bt2020_ncl";
        hlgRush.input_range = "limited";
        const RgbColor hlgThrough =
            TransformColorForOutput(hlgRush, {0.75, 0.75, 0.75});
        Near(hlgThrough.red, 0.75, 0.005,
             "an HLG rush's reference white stays HLG reference white");
        Near(hlgThrough.red, hlgThrough.blue, 2e-5,
             "an HLG neutral stays neutral");
        std::string hlgCube;
        std::string hlgError;
        Check(BuildOpenColorIoCube(hlgRush, hlgCube, hlgError) &&
                  hlgCube.find("LUT_3D_SIZE 65") != std::string::npos,
              "the HLG input builds its OpenColorIO transform: " + hlgError);
        ColorManagementSettings hlgSdr = hlgRush;
        hlgSdr.output_gamut = "rec709";
        hlgSdr.output_transfer = "rec709";
        ColorManagementSettings rec709Approximation = hlgSdr;
        rec709Approximation.input_transfer = "rec709";
        const RgbColor fromHlg =
            TransformColorForOutput(hlgSdr, {0.75, 0.75, 0.75});
        const RgbColor fromApproximation =
            TransformColorForOutput(rec709Approximation, {0.75, 0.75, 0.75});
        Check(std::abs(fromHlg.red - fromApproximation.red) > 0.02,
              "the HLG curve is not the Rec.709 approximation it replaces");

        const ColorManagementSettings preview =
            ColorManagementForSdrPreview(sony);
        Check(preview.enabled && preview.input_gamut == sony.input_gamut &&
                  preview.input_transfer == sony.input_transfer &&
                  preview.working_gamut == sony.working_gamut,
              "SDR preview preserves the input and working transforms");
        Check(preview.output_gamut == "rec709" &&
                  preview.output_transfer == "rec709" &&
                  sony.output_gamut == "rec2020" &&
                  sony.output_transfer == "hlg",
              "SDR preview overrides only its copy of the output transform");
        ColorManagementSettings sdr = sony;
        sdr.output_gamut = "rec709";
        sdr.output_transfer = "rec709";
        const RgbColor compressed =
            MapLinearOutputToDisplay(sdr, {4.0, 1.0, -0.5});
        Check(compressed.red < 1.0 && compressed.red > compressed.green &&
                  compressed.green > compressed.blue && compressed.blue >= 0.0,
              "display rendering compresses highlights and gamut without "
              "independent channel clipping");
        const RgbColor lower = MapLinearOutputToDisplay(sdr, {2.0, 0.8, -0.2});
        Check(compressed.red > lower.red,
              "display shoulder remains continuous instead of flattening "
              "all highlights at code maximum");
        const RgbColor hlgReference =
            MapLinearOutputToDisplay(sony, {0.9, 0.9, 0.9});
        Near(hlgReference.red, 0.9, 1e-12,
             "HLG reference white remains unchanged before encoding");
        const YuvCodeParameters full = BuildYuvCodeParameters(10, true);
        const YuvCodeParameters legal = BuildYuvCodeParameters(10, false);
        Near(full.y_offset, 0.0, 1e-7, "full range starts at code zero");
        Near(legal.y_offset, 64.0 / 1023.0, 1e-7,
             "10-bit legal range starts at code 64");
        Near(legal.y_scale, 1023.0 / 876.0, 1e-7,
             "10-bit legal luma spans 876 codes");
        const YuvMatrixParameters bt709 = BuildYuvMatrixParameters(false);
        const YuvMatrixParameters bt2020 = BuildYuvMatrixParameters(true);
        Near(bt709.red_from_cr, 1.5748, 1e-6,
             "BT.709 uses normalized chroma coefficients once");
        Near(bt2020.red_from_cr, 1.4746, 1e-6,
             "BT.2020 NCL uses its own normalized matrix");
    });

    // G5 -- docs/roadmap/GRADE-2026-09.md.
    Test("scope guides mark what measure_color measures", [] {
        const auto Near = [](double actual, double expected, double epsilon,
                             const std::string& message) {
            Check(std::abs(actual - expected) <= epsilon,
                  message + ": " + std::to_string(actual));
        };
        VideoScopeGuides guides;
        std::string error;
        Check(BuildVideoScopeGuides(ColorManagementSettings{}, guides, error),
              "unmanaged guides build: " + error);
        Near(guides.middle_grey, 0.409, 0.001,
             "without management, grey sits where a Rec.709 camera puts it");
        Near(guides.grey_band[0], 0.383, 0.002,
             "ARRI's grey band, a sixth of a stop under (38 %)");
        Near(guides.grey_band[1], 0.436, 0.002, "and a sixth over (42 %)");
        Check(guides.near_black < guides.grey_band[0] &&
                  guides.grey_band[1] < guides.skin_band[0] &&
                  guides.skin_band[0] < guides.skin_band[1] &&
                  guides.skin_band[1] < guides.near_white,
              "the bands climb without overlapping");
        const auto degrees = [](const std::array<float, 2>& target) {
            double angle = std::atan2(target[1], target[0]) * 180.0 / M_PI;
            return angle < 0.0 ? angle + 360.0 : angle;
        };
        // Where a Cb/Cr scope with Rec.709 weights bins 75 % bars: red at
        // 103 degrees as on any scope, yellow and blue at 174.8 and 354.8,
        // not the 167 and 347 of NTSC's U/V axes.
        Near(degrees(guides.targets[0]), 102.9, 0.1, "red at 102.9");
        Near(degrees(guides.targets[5]), 174.8, 0.1, "yellow at 174.8");
        Near(degrees(guides.targets[2]), 354.8, 0.1, "blue at 354.8");
        Near(std::hypot(guides.targets[0][0], guides.targets[0][1]), 0.385,
             0.002, "75 % red is 0.385 from the centre");
        Near(std::atan2(guides.skin_direction[1], guides.skin_direction[0]) *
                 180.0 / M_PI,
             static_cast<double>(kSkinLineDegrees), 1e-4,
             "the skin line is measure_color's");
        ColorManagementSettings managed;
        managed.enabled = true;
        managed.input_gamut = "sony_sgamut3_cine";
        managed.input_transfer = "sony_slog3";
        VideoScopeGuides aces;
        Check(BuildVideoScopeGuides(managed, aces, error) &&
                  aces.middle_grey > 0.3f && aces.middle_grey < 0.5f &&
                  aces.grey_band[0] < aces.middle_grey &&
                  aces.middle_grey < aces.grey_band[1] &&
                  aces.grey_band[1] < aces.skin_band[0],
              "managed, grey is where the display transform puts it: " +
                  std::to_string(aces.middle_grey) + " " + error);
        managed.output_gamut = "rec2020";
        VideoScopeGuides wide;
        Check(BuildVideoScopeGuides(managed, wide, error) &&
                  std::abs(wide.targets[0][0] - aces.targets[0][0]) > 1e-3,
              "a Rec.2020 output bins with its own luma weights");
    });

    Test("video scope monitoring policy", [] {
        Check(VideoScopeModeFromPreference(-1) == VideoScopeMode::Off &&
                  VideoScopeModeFromPreference(6) == VideoScopeMode::Off,
              "invalid stored scope values fall back to off");
        Check(VideoScopeModeFromPreference(4) == VideoScopeMode::FalseColor &&
                  VideoScopeHistogramBinCount(VideoScopeMode::FalseColor) == 1,
              "false colour is a stored mode, and bins nothing");
        Check(VideoScopeModeFromPreference(2) == VideoScopeMode::ParadeRgb,
              "stored RGB parade preference round trips");
        Check(VideoScopeModeFromPreference(5) == VideoScopeMode::WaveformRgb &&
                  VideoScopeHistogramBinCount(VideoScopeMode::WaveformRgb) ==
                      3u * 256u * 128u,
              "the RGB waveform is stored as 5 and bins one plane per "
              "channel over the waveform's width");
        Check(VideoScopeHistogramBinCount(VideoScopeMode::Waveform) ==
                      256u * 128u &&
                  VideoScopeHistogramBinCount(VideoScopeMode::ParadeRgb) ==
                      3u * 128u * 128u &&
                  VideoScopeHistogramBinCount(VideoScopeMode::Vectorscope) ==
                      256u * 256u,
              "each scope owns the expected histogram layout");
    });

    // PRD37 -- the QA of 3 October 2026 saw the scope selector cover the
    // darkest fifth of a parade: the panel goes above the controls' strip.
    Test("the scope panel sits above the monitor's controls", [] {
        // Record at 2x in the QA window: 970 x 812 pixels.
        const VideoScopePanel bare = LayoutVideoScopePanel(970.0, 812.0, 0.0);
        Check(bare.x == kVideoScopeMarginPixels &&
                  std::abs(bare.width - 407.4) < 1e-9 &&
                  std::abs(bare.height - 407.4 * 0.62) < 1e-9 &&
                  std::abs(bare.y + bare.height - (812.0 - 12.0)) < 1e-9,
              "without a strip, the panel is the one drawn since UI-2026-08");
        // The selector's top, 28 points up at 2x.
        const VideoScopePanel above = LayoutVideoScopePanel(970.0, 812.0, 56.0);
        Check(std::abs(above.y + above.height - (812.0 - 56.0 - 12.0)) < 1e-9 &&
                  above.width == bare.width && above.height == bare.height,
              "with one, the same panel, its foot a margin above the strip");
        const VideoScopePanel low = LayoutVideoScopePanel(970.0, 120.0, 56.0);
        Check(std::abs(low.height - 40.0) < 1e-9 &&
                  std::abs(low.y - kVideoScopeMarginPixels) < 1e-9,
              "a short monitor shrinks the panel into what is left above the "
              "strip, never under the controls nor off the top");
        const VideoScopePanel none = LayoutVideoScopePanel(970.0, 60.0, 56.0);
        Check(none.width == 0.0 && none.height == 0.0,
              "no room above the strip, no panel");
        Check(LayoutVideoScopePanel(0.0, 812.0, 0.0).width == 0.0 &&
                  LayoutVideoScopePanel(970.0, 812.0, -40.0).y == bare.y,
              "no drawable, no panel; a negative strip is no strip");
    });

    Test("RationalTime arithmetic across rates", [] {
        const RationalTime a{1, 24};
        const RationalTime b{1, 48};
        const RationalTime sum = a.add(b);
        Check(sum.value == 3 && sum.rate == 48, "1/24 + 1/48 must be 3/48");
        Check(a.compare(RationalTime{2, 48}) == 0,
              "equivalent rates compare equal");
        Check(sum.sub(a) == b, "sub must preserve the exact rational result");
        Check(a.rescale(48).value == 2, "exact rescale must produce 2/48");
        Check(RationalTime{1001, 30000}.to_frames(30000, 1001) == 1,
              "30000/1001 frame mapping must remain integral");
        const Ulid generated = GenerateUlid();
        Check(IsValidUlid(generated), "generated IDs must be valid ULIDs");
        Check(generated != GenerateUlid(),
              "successive generated ULIDs must differ");
    });

    Test("load/save/load canonical round trip", [] {
        const Document original = ValidDocument();
        std::string error;
        Check(original.Validate(error), "fixture must validate: " + error);
        const std::filesystem::path first =
            std::filesystem::temp_directory_path() / (GenerateUlid() + ".json");
        const std::filesystem::path second =
            std::filesystem::temp_directory_path() / (GenerateUlid() + ".json");
        Check(original.Save(first.string(), error), "first save: " + error);
        Document loaded;
        Check(Document::Load(first.string(), loaded, error), "load: " + error);
        Check(loaded.Save(second.string(), error), "second save: " + error);
        Document loadedAgain;
        Check(Document::Load(second.string(), loadedAgain, error),
              "second load: " + error);
        Check(loaded.SaveToString() == loadedAgain.SaveToString(),
              "canonical JSON must be byte-identical after second load");
        std::filesystem::remove(first);
        std::filesystem::remove(second);
    });

    Test("legacy document versions are rejected", [] {
        const std::string json =
            "{\"version\":1,\"sources\":[{\"id\":"
            "\"01K90000000000000000000001\",\"path\":\"legacy.mov\","
            "\"rate\":{\"num\":25,\"den\":1},\"duration\":"
            "{\"value\":100,\"rate\":25}}],\"tracks\":[]}";
        Document document;
        std::string error;
        Check(!Document::LoadFromString(json, document, error) &&
                  error.find("unsupported document version 1") !=
                      std::string::npos,
              "version 1 must be rejected explicitly");
    });

    Test("version 2 root timelines are rejected", [] {
        const std::string json =
            "{\"version\":2,\"sequence\":{\"id\":"
            "\"01K91000000000000000000001\",\"name\":\"Legacy edit\","
            "\"width\":1920,\"height\":1080,\"frame_rate\":{\"num\":25,"
            "\"den\":1}},\"library\":[],\"bins\":[],\"markers\":[{"
            "\"id\":\"01K91000000000000000000002\",\"name\":\"Cut\","
            "\"time\":{\"value\":10,\"rate\":25},\"color\":\"yellow\","
            "\"category\":\"edit\"}],\"sources\":[],\"tracks\":[]}";
        Document document;
        std::string error;
        Check(!Document::LoadFromString(json, document, error) &&
                  error.find("unsupported document version 2") !=
                      std::string::npos,
              "version 2 must be rejected explicitly");
    });

    Test("sequence settings persist and validate", [] {
        Document document = ValidDocument();
        document.sequence.name = "Vertical master";
        document.sequence.width = 1080;
        document.sequence.height = 1920;
        document.sequence.frame_rate = {30000, 1001};
        std::string error;
        Document loaded;
        Check(Document::LoadFromString(document.SaveToString(), loaded, error),
              "sequence settings load: " + error);
        Check(loaded.sequence.id == document.sequence.id &&
                  loaded.sequence.name == "Vertical master" &&
                  loaded.sequence.width == 1080 &&
                  loaded.sequence.height == 1920 &&
                  loaded.sequence.frame_rate.num == 30000 &&
                  loaded.sequence.frame_rate.den == 1001,
              "sequence identity, dimensions and rational cadence round-trip");
        loaded.sequence.width = 0;
        Check(!loaded.Validate(error) &&
                  error.find("sequence") != std::string::npos,
              "invalid sequence dimensions are rejected");
    });

    Test("marker model persists exact time and validates identity", [] {
        Document document = ValidDocument();
        document.sequence.markers = {
            {"01K83000000000000000000001",
             "Act 1",
             {1001, 24000},
             "#ffcc00",
             "chapter"},
            {"01K83000000000000000000002",
             "Needs sound",
             {73, 25},
             "violet",
             "todo"},
        };
        std::string error;
        Document loaded;
        const std::string canonical = document.SaveToString();
        Check(Document::LoadFromString(canonical, loaded, error),
              "markers load from canonical JSON: " + error);
        Check(loaded.SaveToString() == canonical &&
                  loaded.sequence.markers.size() == 2,
              "marker document JSON round-trips byte-identically");
        Check(loaded.FindMarker(document.sequence.markers[0].id) &&
                  loaded.FindMarker(document.sequence.markers[0].id)->time ==
                      RationalTime{1001, 24000},
              "marker lookup retains exact rational time");

        loaded.sequence.markers[0].id = loaded.sequence.tracks[0].id;
        Check(!loaded.Validate(error) &&
                  error.find("duplicate ID") != std::string::npos,
              "marker IDs participate in global identity validation");
        loaded = document;
        loaded.sequence.markers[0].category.clear();
        Check(!loaded.Validate(error) &&
                  error.find("marker") != std::string::npos,
              "empty marker categories are rejected");
    });

    Test("color management settings persist and validate", [] {
        Document document = ValidDocument();
        document.color_management.enabled = true;
        document.color_management.input_gamut = "sony_sgamut3_cine";
        document.color_management.input_transfer = "sony_slog3";
        document.color_management.input_ycbcr_matrix = "bt709";
        document.color_management.input_range = "full";
        document.color_management.working_gamut = "acescct";
        document.color_management.output_gamut = "rec2020";
        document.color_management.output_transfer = "hlg";
        std::string error;
        Check(document.Validate(error),
              "Sony/HLG pipeline must validate: " + error);
        Document loaded;
        Check(Document::LoadFromString(document.SaveToString(), loaded, error),
              "Sony/HLG pipeline must load: " + error);
        Check(loaded.color_management.enabled &&
                  loaded.color_management.input_gamut == "sony_sgamut3_cine" &&
                  loaded.color_management.input_transfer == "sony_slog3" &&
                  loaded.color_management.input_range == "full" &&
                  loaded.color_management.working_gamut == "acescct" &&
                  loaded.color_management.output_gamut == "rec2020" &&
                  loaded.color_management.output_transfer == "hlg",
              "all color pipeline stages must round-trip");

        OpenColorIoLutPair luts;
        Check(BuildOpenColorIoLuts(document.color_management, luts, error),
              "embedded OCIO config must build Metal LUTs: " + error);
        const size_t expectedValues = static_cast<size_t>(kOpenColorIoLutEdge) *
                                      kOpenColorIoLutEdge *
                                      kOpenColorIoLutEdge * 4;
        Check(luts.input_to_working.edge == kOpenColorIoLutEdge &&
                  luts.working_to_display.edge == kOpenColorIoLutEdge &&
                  luts.input_to_working.rgba.size() == expectedValues &&
                  luts.working_to_display.rgba.size() == expectedValues,
              "OCIO LUTs use deterministic 65^3 RGBA storage");

        ColorManagementSettings legacyRec2020 = document.color_management;
        legacyRec2020.input_gamut = "rec2020";
        legacyRec2020.input_transfer = "rec709";
        Check(BuildOpenColorIoLuts(legacyRec2020, luts, error),
              "legacy Rec.2020/Rec.709 input builds through OCIO: " + error);

        loaded.color_management.output_gamut = "rec709";
        Check(!loaded.Validate(error) && error.find("HLG") != std::string::npos,
              "HLG with a non-Rec.2020 gamut must be rejected");
        // ITW5 -- an HLG input is BT.2100: Rec.2020 primaries only.
        Document hlgInput = document;
        hlgInput.color_management.input_gamut = "rec2020";
        hlgInput.color_management.input_transfer = "hlg";
        Check(hlgInput.Validate(error),
              "a Rec.2020 HLG input validates: " + error);
        hlgInput.color_management.input_gamut = "rec709";
        Check(!hlgInput.Validate(error),
              "an HLG input with Rec.709 primaries is refused");
        loaded.color_management.output_gamut = "rec2020";
        loaded.color_management.input_transfer = "unknown_log";
        Check(!loaded.Validate(error) &&
                  error.find("color_management") != std::string::npos,
              "unknown transfer functions must be rejected");
        loaded = document;
        loaded.color_management.input_gamut = "rec2020";
        Check(!loaded.Validate(error) &&
                  error.find("OpenColorIO") != std::string::npos,
              "unsupported OCIO gamut/transfer pairs are rejected early");
    });

    Test("timeline resolution", [] {
        const Document document = ValidDocument();
        Timeline timeline(document);
        const Ulid& primaryTrack = document.sequence.tracks[0].id;
        const Ulid& sparseTrack = document.sequence.tracks[1].id;

        const auto first = timeline.ResolveTrack(primaryTrack, {0, 25});
        Check(first && first->source_id == document.sources[0].id &&
                  first->source_frame == 100,
              "first timeline frame must map to first clip source frame 100");

        const auto cut = timeline.ResolveTrack(primaryTrack, {2, 25});
        Check(cut && cut->source_id == document.sources[1].id &&
                  cut->source_frame == 10,
              "half-open cut boundary must map to second clip");

        Check(!timeline.ResolveTrack(sparseTrack, {0, 25}),
              "position before sparse clip must resolve to a hole");

        const auto last = timeline.ResolveTrack(primaryTrack, {3, 25});
        Check(last && last->source_id == document.sources[1].id &&
                  last->source_frame == 11,
              "last timeline frame must map to second clip source frame 11");
        Check(!timeline.ResolveTrack(primaryTrack, {4, 25}),
              "timeline end is exclusive");

        const auto allTracks = timeline.Resolve({0, 25});
        Check(
            allTracks.size() == 2 && allTracks[0].frame && !allTracks[1].frame,
            "Resolve must return one populated-or-hole result per track");
    });

    Test(
        "upcoming cuts resolve the frame each decoder must be warmed with", [] {
            const Document document = ValidDocument();
            Timeline timeline(document);
            const Ulid& firstSource = document.sources[0].id;
            const Ulid& secondSource = document.sources[1].id;

            const auto ahead = timeline.ResolveUpcoming({0, 25}, 1, {2, 25});
            Check(ahead.size() == 2 && ahead[0].source_id == secondSource &&
                      ahead[0].source_frame == 10 &&
                      ahead[1].source_id == firstSource &&
                      ahead[1].source_frame == 0,
                  "a cut ahead resolves the incoming clip's first frame, one "
                  "entry per track");

            const auto near = timeline.ResolveUpcoming({0, 25}, 1, {1, 25});
            Check(near.size() == 1 && near[0].source_id == firstSource &&
                      near[0].source_frame == 0,
                  "a cut beyond the lookahead is not warmed yet");

            Check(timeline.ResolveUpcoming({2, 25}, 1, {2, 25}).empty(),
                  "no cut ahead means nothing to warm");
            Check(timeline.ResolveUpcoming({0, 25}, 0, {2, 25}).empty(),
                  "a stopped playhead warms nothing");
            Check(timeline.ResolveUpcoming({0, 25}, 1, {0, 25}).empty(),
                  "an empty lookahead warms nothing");

            const auto back = timeline.ResolveUpcoming({3, 25}, -1, {2, 25});
            Check(back.size() == 2 && back[0].source_id == firstSource &&
                      back[0].source_frame == 101 &&
                      back[1].source_id == firstSource &&
                      back[1].source_frame == 0,
                  "played backward, a clip is entered by its last frame");

            const auto backNear =
                timeline.ResolveUpcoming({4, 25}, -1, {1, 25});
            Check(backNear.size() == 1 &&
                      backNear[0].source_id == secondSource &&
                      backNear[0].source_frame == 11,
                  "backward lookahead stops at the same distance forward "
                  "lookahead does");

            // Same set of tracks main.mm composites from
            // (rebuildVideoTrackIds).
            Document hidden = document;
            hidden.sequence.tracks[1].visible = false;
            Check(
                Timeline(hidden).ResolveUpcoming({0, 25}, 1, {2, 25}).size() ==
                    1,
                "a hidden track feeds no layer, so it warms no decoder");
            Document audio = document;
            audio.sequence.tracks[1].kind = "audio";
            Check(Timeline(audio).ResolveUpcoming({0, 25}, 1, {2, 25}).size() ==
                      1,
                  "an audio track warms no video decoder");
        });

    Test(
        "cross dissolve persists, validates handles and resolves two layers",
        [] {
            Document document = ValidDocument();
            document.sequence.tracks[0].clips[0].opacity = {4, 5};
            document.sequence.tracks[0].clips[1].opacity = {3, 5};
            document.sequence.transitions = {{
                "01K00000000000000000000008",
                document.sequence.tracks[0].id,
                document.sequence.tracks[0].clips[0].id,
                document.sequence.tracks[0].clips[1].id,
                "cross_dissolve",
                {2, 25},
                TransitionAlignment::Center,
            }};
            std::string error;
            Check(document.Validate(error),
                  "transition fixture validates: " + error);
            Document loaded;
            const std::string json = document.SaveToString();
            Check(Document::LoadFromString(json, loaded, error) &&
                      loaded.SaveToString() == json &&
                      loaded.FindTransition("01K00000000000000000000008"),
                  "transition JSON round-trips canonically");

            Timeline timeline(document);
            const Ulid track = document.sequence.tracks[0].id;
            const auto beforeCut = timeline.ResolveTrackLayers(track, {1, 25});
            Check(beforeCut.size() == 2 &&
                      beforeCut[0].frame.source_frame == 101 &&
                      beforeCut[1].frame.source_frame == 9 &&
                      std::abs(beforeCut[0].opacity - 0.8f) < 0.0001f &&
                      beforeCut[1].opacity == 0.0f,
                  "transition begins with outgoing image and incoming head "
                  "handle");
            const auto atCut = timeline.ResolveTrackLayers(track, {2, 25});
            Check(atCut.size() == 2 && atCut[0].frame.source_frame == 102 &&
                      atCut[1].frame.source_frame == 10 &&
                      std::abs(atCut[0].opacity - 0.8f) < 0.0001f &&
                      std::abs(atCut[1].opacity - 0.3f) < 0.0001f,
                  "cut center resolves both source handles at 50 percent");
            const auto after = timeline.ResolveTrackLayers(track, {3, 25});
            Check(
                after.size() == 1 && after[0].frame.source_frame == 11 &&
                    std::abs(after[0].opacity - 0.6f) < 0.0001f,
                "transition end is half-open and returns to normal resolution");

            document.sequence.tracks[0].clips[1].source_in = {0, 25};
            Check(!document.Validate(error) &&
                      error.find("media handles") != std::string::npos,
                  "a centered dissolve without an incoming head handle is "
                  "rejected");
        });

    Test("mixed-rate cut resolution at the common timebase", [] {
        Document document;
        document.sources = {
            {"01K10000000000000000000001", "25.MP4", {25, 1}, {250, 25}},
            {"01K10000000000000000000002",
             "2997.MP4",
             {30000, 1001},
             {300300, 30000}},
        };
        document.sequence.tracks = {
            {"01K10000000000000000000003",
             "video",
             0,
             {
                 {"01K10000000000000000000004",
                  "01K10000000000000000000001",
                  {0, 25},
                  {25, 25},
                  {0, 25}},
                 {"01K10000000000000000000005",
                  "01K10000000000000000000002",
                  {100100, 30000},
                  {1001, 30000},
                  {30000, 30000}},
             }},
        };
        std::string error;
        Check(document.Validate(error),
              "mixed-rate fixture must validate: " + error);
        Timeline timeline(document);
        Check(timeline.Duration().rate == 30000,
              "timeline timebase must be the exact LCM 30000");

        const auto before = timeline.ResolveTrack(
            document.sequence.tracks[0].id, {29999, 30000});
        Check(before && before->source_id == document.sources[0].id &&
                  before->source_frame == 24,
              "tick before cut must remain on the last 25 fps frame");
        const auto atCut = timeline.ResolveTrack(document.sequence.tracks[0].id,
                                                 {30000, 30000});
        Check(atCut && atCut->source_id == document.sources[1].id &&
                  atCut->source_frame == 100,
              "cut tick must resolve to the first 30000/1001 clip frame");
    });

    Test("document validation", [] {
        {
            Document value = ValidDocument();
            value.sequence.tracks[0].id = value.sources[0].id;
            ExpectInvalid(value, "duplicate ID", "duplicate IDs");
        }
        {
            Document value = ValidDocument();
            value.sequence.tracks[0].clips[0].source_id =
                "01K00000000000000000000009";
            ExpectInvalid(value, "unknown source_id", "unknown source_id");
        }
        {
            Document value = ValidDocument();
            value.sequence.tracks[0].clips[1].timeline_in = {1, 25};
            ExpectInvalid(value, "overlap", "overlapping clips");
        }
        {
            Document value = ValidDocument();
            std::swap(value.sequence.tracks[0].clips[0],
                      value.sequence.tracks[0].clips[1]);
            ExpectInvalid(value, "not sorted", "unsorted clips");
        }
        {
            Document value = ValidDocument();
            value.sequence.tracks[0].clips[0].duration.value = 0;
            ExpectInvalid(value, "zero or negative duration", "zero duration");
        }
        {
            Document value = ValidDocument();
            value.sequence.tracks[0].clips[0].duration.value = -1;
            ExpectInvalid(value, "zero or negative duration",
                          "negative duration");
        }
        {
            Document value = ValidDocument();
            value.sequence.tracks[0].clips[0].source_in = {199, 25};
            ExpectInvalid(value, "outside source bounds", "source bounds");
        }
        {
            Document value = ValidDocument();
            value.sources[0].rate.num = 0;
            ExpectInvalid(value, "media rate", "zero source media rate");
        }
        {
            Document value = ValidDocument();
            value.sequence.tracks[0].clips[0].timeline_in.rate = 0;
            ExpectInvalid(value, "time rate", "zero RationalTime rate");
        }
    });

    Test("clip color effects persist and validate", [] {
        Document document = ValidDocument();
        document.sequence.tracks[0].clips[0].effects = {
            {"01K84000000000000000000001",
             "color.exposure",
             {{"amount", {35, 100}}}},
            {"01K84000000000000000000002",
             "color.temperature",
             {{"kelvin", {5600, 1}}, {"tint", {-5, 100}}}},
        };
        std::string error;
        Check(document.Validate(error), "effect fixture validates: " + error);
        const std::string canonical = document.SaveToString();
        Document loaded;
        Check(Document::LoadFromString(canonical, loaded, error),
              "clip effects load from canonical JSON: " + error);
        Check(loaded.SaveToString() == canonical,
              "clip effect document JSON round-trips byte-identically");
        Check(loaded.sequence.tracks[0].clips[0].effects.size() == 2 &&
                  loaded.sequence.tracks[0].clips[0].effects[0].type ==
                      "color.exposure",
              "effect stack keeps its order and type");

        loaded = document;
        loaded.sequence.tracks[0].clips[0].effects[0].type = "brightness";
        Check(!loaded.Validate(error) &&
                  error.find("unsupported effect type") != std::string::npos,
              "effect types outside the color.* family are rejected");

        loaded = document;
        loaded.sequence.tracks[0].clips[0].effects[0].params["amount"].den = 0;
        Check(!loaded.Validate(error) &&
                  error.find("non-positive denominator") != std::string::npos,
              "effect parameters must have a positive denominator");

        loaded = document;
        loaded.sequence.tracks[1].clips[0].effects =
            document.sequence.tracks[0].clips[0].effects;
        Check(!loaded.Validate(error) &&
                  error.find("duplicate ID") != std::string::npos,
              "effect IDs participate in global identity validation");

        loaded = document;
        loaded.sequence.tracks[1].kind = "audio";
        loaded.sequence.tracks[1].clips[0].effects = {
            {"01K84000000000000000000003", "color.exposure", {}}};
        Check(!loaded.Validate(error) &&
                  error.find("non-video track") != std::string::npos,
              "color effects are rejected on non-video tracks");

        // F1.3 -- wheels and curves are scalar params of the same schema:
        // they persist and round-trip with no format change, and their
        // shape is checked where every effect is accepted.
        loaded = document;
        loaded.sequence.tracks[0].clips[0].effects.push_back(
            {"01K84000000000000000000004",
             "color.wheels",
             {{"lift_r", {1, 20}}, {"gamma_master", {-1, 4}}}});
        loaded.sequence.tracks[0].clips[0].effects.push_back(
            {"01K84000000000000000000005",
             "color.curves",
             {{"master_x0", {0, 1}},
              {"master_y0", {0, 1}},
              {"master_x1", {1, 2}},
              {"master_y1", {3, 5}},
              {"master_x2", {1, 1}},
              {"master_y2", {1, 1}}}});
        Check(loaded.Validate(error), "wheels and curves validate: " + error);
        const std::string graded = loaded.SaveToString();
        Document reloaded;
        Check(Document::LoadFromString(graded, reloaded, error) &&
                  reloaded.SaveToString() == graded,
              "wheels and curves round-trip byte-identically: " + error);
        Document refused = loaded;
        refused.sequence.tracks[0].clips[0].effects[3].params["master_x1"] = {
            1, 1};
        Check(!refused.Validate(error) &&
                  error.find("effect 3 ('01K84000000000000000000005')") !=
                      std::string::npos &&
                  error.find("not increasing in x") != std::string::npos,
              "a curve that goes back in x is refused by the document, "
              "naming the effect: " +
                  error);
        refused = loaded;
        refused.sequence.tracks[0].clips[0].effects[2].params["lift_r"] = {3,
                                                                           2};
        Check(
            !refused.Validate(error) &&
                error.find("outside [-1, 1]") != std::string::npos,
            "a wheel knob past its bound is refused by the document: " + error);
    });

    Test("caption schema persists and validates", [] {
        Document document = ValidDocument();
        const Ulid styleId = "01K85000000000000000000001";
        document.sequence.caption_styles = {
            {styleId, "Inter", 64, "#ffffff", "bottom"},
        };
        document.sequence.tracks[0].clips[0].caption_group_id = styleId;
        document.sequence.tracks[0].clips[0].caption_text = "Bonjour";
        std::string error;
        Check(document.Validate(error), "caption fixture validates: " + error);
        const std::string canonical = document.SaveToString();
        Document loaded;
        Check(Document::LoadFromString(canonical, loaded, error),
              "caption document loads from canonical JSON: " + error);
        Check(loaded.SaveToString() == canonical,
              "caption document JSON round-trips byte-identically");
        Check(loaded.FindCaptionStyle(styleId) &&
                  loaded.sequence.tracks[0].clips[0].caption_text == "Bonjour",
              "caption style and per-clip text round-trip");

        loaded = document;
        loaded.sequence.tracks[0].clips[0].caption_group_id =
            "01K85000000000000000000099";
        Check(!loaded.Validate(error) &&
                  error.find("unknown caption_group_id") != std::string::npos,
              "a clip referencing an unknown caption style is rejected");

        loaded = document;
        loaded.sequence.caption_styles[0].font_size = 0;
        Check(!loaded.Validate(error) &&
                  error.find("caption style") != std::string::npos,
              "an invalid caption style is rejected");
    });

    Test("multicam group schema persists and validates", [] {
        Document document = ValidDocument();
        const Ulid groupId = "01K86000000000000000000001";
        const Ulid angleAId = "01K86000000000000000000002";
        const Ulid angleBId = "01K86000000000000000000003";
        document.sequence.multicam_groups = {
            {groupId,
             "Interview",
             {
                 {angleAId, "Wide", document.sequence.tracks[0].clips[0].id},
                 {angleBId, "Close", document.sequence.tracks[0].clips[1].id},
             },
             angleAId},
        };
        std::string error;
        Check(document.Validate(error), "multicam fixture validates: " + error);
        const std::string canonical = document.SaveToString();
        Document loaded;
        Check(Document::LoadFromString(canonical, loaded, error),
              "multicam document loads from canonical JSON: " + error);
        Check(loaded.SaveToString() == canonical,
              "multicam document JSON round-trips byte-identically");
        Check(loaded.FindMulticamGroup(groupId) &&
                  loaded.FindMulticamGroup(groupId)->angles.size() == 2,
              "multicam group and its angles round-trip");

        loaded = document;
        loaded.sequence.multicam_groups[0].active_angle_id =
            "01K86000000000000000000099";
        Check(!loaded.Validate(error) &&
                  error.find("does not reference one of its own angles") !=
                      std::string::npos,
              "an active angle outside the group is rejected");

        loaded = document;
        loaded.sequence.multicam_groups[0].angles[1].clip_id =
            document.sequence.tracks[0].clips[0].id;
        Check(!loaded.Validate(error) &&
                  error.find("already used by another angle") !=
                      std::string::npos,
              "two angles cannot reference the same clip");

        loaded = document;
        loaded.sequence.multicam_groups[0].angles[0].clip_id =
            "01K86000000000000000000098";
        Check(!loaded.Validate(error) &&
                  error.find("must reference a clip on a video track") !=
                      std::string::npos,
              "an angle must reference an existing clip on a video track");
    });

    Test("older document schemas migrate audio settings to neutral", [] {
        const std::string canonical = ValidDocument().SaveToString();
        std::string json = canonical;
        const std::string current = "\"version\": 7";
        const size_t versionPosition = json.find(current);
        Check(versionPosition != std::string::npos,
              "canonical fixture contains its schema version");
        json.replace(versionPosition, current.size(), "\"version\": 4");
        Document document;
        std::string error;
        const std::string audioGain =
            ",\"audio_gain_db\":{\"num\":0,\"den\":1}";
        const std::string audioFadeIn =
            ",\"audio_fade_in\":{\"value\":0,\"rate\":1}";
        const std::string audioFadeOut =
            ",\"audio_fade_out\":{\"value\":0,\"rate\":1}";
        for (const std::string& field :
             {audioGain, audioFadeIn, audioFadeOut}) {
            size_t position = 0;
            while ((position = json.find(field, position)) != std::string::npos)
                json.erase(position, field.size());
        }
        const std::string opacity = ",\"opacity\":{\"num\":1,\"den\":1}";
        size_t opacityPosition = 0;
        while ((opacityPosition = json.find(opacity, opacityPosition)) !=
               std::string::npos)
            json.erase(opacityPosition, opacity.size());
        Check(Document::LoadFromString(json, document, error),
              "version 4 loads through neutral audio migration: " + error);
        Check(document.version == 7,
              "the migrated document uses the current schema version");
        bool neutralAudio = true;
        for (const DocumentTrack& track : document.sequence.tracks) {
            for (const DocumentClip& clip : track.clips) {
                neutralAudio = neutralAudio && clip.audio_gain_db.num == 0 &&
                               clip.audio_gain_db.den == 1 &&
                               clip.audio_fade_in == RationalTime{0, 1} &&
                               clip.audio_fade_out == RationalTime{0, 1};
            }
        }
        Check(neutralAudio,
              "every migrated clip defaults to neutral exact audio settings");
        Check(
            document.SaveToString().find("\"version\": 7") != std::string::npos,
            "saving a migrated document emits canonical version 7");

        const auto withoutAudioSettings = [&](int version) {
            std::string legacy = canonical;
            legacy.replace(versionPosition, current.size(),
                           "\"version\": " + std::to_string(version));
            for (const std::string& field :
                 {audioGain, audioFadeIn, audioFadeOut}) {
                size_t position = 0;
                while ((position = legacy.find(field, position)) !=
                       std::string::npos)
                    legacy.erase(position, field.size());
            }
            return legacy;
        };
        Document v5;
        const std::string v5Json = withoutAudioSettings(5);
        Check(Document::LoadFromString(v5Json, v5, error) &&
                  v5.sequence.tracks[0].clips[0].opacity.num == 1 &&
                  v5.sequence.tracks[0].clips[0].opacity.den == 1 &&
                  v5.sequence.tracks[0].clips[0].audio_gain_db.num == 0 &&
                  v5.sequence.tracks[0].clips[0].audio_gain_db.den == 1,
              "version 5 preserves opacity and adds neutral audio settings: " +
                  error);
        Document v6;
        const std::string v6Json = withoutAudioSettings(6);
        Check(Document::LoadFromString(v6Json, v6, error) &&
                  v6.sequence.tracks[0].clips[0].audio_fade_in ==
                      RationalTime{0, 1} &&
                  v6.sequence.tracks[0].clips[0].audio_fade_out ==
                      RationalTime{0, 1},
              "version 6 adds neutral audio settings: " + error);
    });

    // QC-2026-09 A3 -- the measured audio level is a document fact, so it has
    // to survive the canonical round trip, and a v5 library entry that never
    // saw it has to load as unmeasured rather than as digital silence. Those
    // are the same zero and not the same statement.
    Test("a measured audio level round-trips, and its absence stays unknown",
         [] {
             Document document = ValidDocument();
             LibraryMedia media;
             media.id = document.sources[0].id;
             media.path = document.sources[0].path;
             media.filename = "A.MP4";
             media.codec = "h264";
             media.width = 1920;
             media.height = 1080;
             media.pixel_format = "yuv420p";
             media.orientation = "landscape";
             media.rate = document.sources[0].rate;
             media.duration = document.sources[0].duration;
             media.has_audio = true;
             media.audio_rate = 48000;
             media.audio_channels = 2;
             media.audio_level_measured = true;
             media.audio_level = 200;  // -74 dBFS: a mute cutaway.
             document.library = {media};

             std::string error;
             Check(document.Validate(error),
                   "a measured level validates: " + error);
             const std::string json = document.SaveToString();
             Check(json.find("\"audio_level\":200") != std::string::npos,
                   "the level reaches the canonical bytes");
             Document reloaded;
             Check(Document::LoadFromString(json, reloaded, error) &&
                       reloaded.library.size() == 1 &&
                       reloaded.library[0].audio_level_measured &&
                       reloaded.library[0].audio_level == 200,
                   "and reads back unchanged: " + error);
             Check(reloaded.SaveToString() == json,
                   "a second save is byte-identical");

             document.library[0].audio_level_measured = false;
             document.library[0].audio_level = 0;
             const std::string silent = document.SaveToString();
             Check(silent.find("audio_level") == std::string::npos,
                   "an unmeasured entry keeps the bytes it had before the "
                   "field existed");
             Document legacy;
             Check(Document::LoadFromString(silent, legacy, error) &&
                       !legacy.library[0].audio_level_measured,
                   "and loads back as unknown, not as silence: " + error);

             document.library[0].audio_level_measured = true;
             document.library[0].audio_level = kAudioLevelScale + 1;
             Check(!document.Validate(error) &&
                       error.find("audio level") != std::string::npos,
                   "a level above full scale is refused rather than stored");
         });

    Test("track lock state persists", [] {
        Document document = ValidDocument();
        document.sequence.tracks[0].locked = true;
        document.sequence.tracks[0].sync_lock = false;
        Document loaded;
        std::string error;
        Check(Document::LoadFromString(document.SaveToString(), loaded, error),
              "locked document loads: " + error);
        Check(loaded.sequence.tracks[0].locked,
              "locked state survives canonical JSON");
        Check(!loaded.sequence.tracks[0].sync_lock,
              "sync lock state survives canonical JSON");
    });

    Test("derived proxy path persists without replacing the original", [] {
        Document document = ValidDocument();
        LibraryMedia media;
        media.id = document.sources[0].id;
        media.path = document.sources[0].path;
        media.filename = "A.MP4";
        media.codec = "h264";
        media.width = 3840;
        media.height = 2160;
        media.rate = document.sources[0].rate;
        media.duration = document.sources[0].duration;
        media.orientation = "landscape";
        document.library.push_back(std::move(media));
        const std::string original = document.library[0].path;
        document.library[0].proxy_path =
            ".cutmachine/proxies/01K00000000000000000000001.mov";
        Document loaded;
        std::string error;
        Check(Document::LoadFromString(document.SaveToString(), loaded, error),
              "document with proxy loads: " + error);
        Check(
            loaded.library[0].path == original &&
                loaded.library[0].proxy_path == document.library[0].proxy_path,
            "proxy remains derived metadata beside the original path");
    });

    if (failures != 0) {
        std::cerr << failures << " assertion(s) failed\n";
        return 1;
    }
    std::cout << "All model tests passed\n";
    return 0;
}
