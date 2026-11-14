#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct ColorManagementSettings;

struct RgbColor {
    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;
};

struct YuvCodeParameters {
    float sample_scale = 1.0f;
    float y_offset = 0.0f;
    float y_scale = 1.0f;
    float chroma_offset = 0.5f;
    float chroma_scale = 1.0f;
};

struct YuvMatrixParameters {
    float red_from_cr = 1.5748f;
    float green_from_cb = -0.187324f;
    float green_from_cr = -0.468124f;
    float blue_from_cb = 1.8556f;
};

// COLOR-2026-08 -- the OCIO library and config are both pinned because their
// output is part of the deterministic render contract. The config is compiled
// into OpenColorIO itself, so rendering never consults $OCIO or the filesystem.
inline constexpr const char* kOpenColorIoConfig =
    "studio-config-v2.2.0_aces-v1.3_ocio-v2.4";
inline constexpr int32_t kOpenColorIoLutEdge = 65;

struct OpenColorIoLut3D {
    int32_t edge = 0;
    // R is the fastest-varying coordinate. RGBA storage maps directly to a
    // Metal RGBA32Float 3D texture; alpha remains one.
    std::vector<float> rgba;
};

struct OpenColorIoLutPair {
    OpenColorIoLut3D input_to_working;
    OpenColorIoLut3D working_to_display;
};

YuvCodeParameters BuildYuvCodeParameters(int bitDepth, bool fullRange);
YuvMatrixParameters BuildYuvMatrixParameters(bool bt2020NonConstant);

// How the monitor reads a decoded picture's YCbCr range and matrix
// (Renderer.mm), written once so the colour measure (G1 --
// docs/roadmap/GRADE-2026-09.md) decodes the same code values the same way.
// Under "auto", a picture is full range when tagged so, or when untagged
// with an S-Log3 input; it takes the BT.2020 matrix when tagged BT.2020
// non-constant luminance, BT.709 otherwise. An RGB picture has no range of
// its own and reads as full. A declared range or matrix wins over any tag.
enum class YuvRangeTag { Unspecified, Limited, Full };
bool ResolveYuvFullRange(const ColorManagementSettings& settings,
                         YuvRangeTag tag, bool rgb);
bool ResolveYuvBt2020Matrix(const ColorManagementSettings& settings,
                            bool taggedBt2020NonConstant);

double DecodeSonySLog3(double signal);
double EncodeAcesCct(double linearAp1);
double DecodeAcesCct(double acesCct);
// GR16 -- docs/roadmap/GRADE-RESOLVE-2026-10.md. DaVinci Intermediate, the
// log encoding a Resolve timeline in DaVinci Wide Gamut grades on, from
// Blackmagic's published formula: linear below 0.00262409, log2 above.
double EncodeDaVinciIntermediate(double linear);
double DecodeDaVinciIntermediate(double code);
double EncodeHlg(double sceneLinear);
double DecodeHlg(double signal);

// Scene-reflection scale that maps Sony's 90% white to BT.2408 HLG
// Reference White at signal 0.75.
double HlgSceneReflectionScale();

// Display rendering stage applied after AP1 has been converted into the
// output primaries. It provides a luminance shoulder and hue-preserving gamut
// compression before the transfer function is encoded. This is deliberately
// not named an ACES Output Transform: CUTMACHINE does not yet ship the full
// ACES 2.0 JMh transform or an OCIO processor.
RgbColor MapLinearOutputToDisplay(const ColorManagementSettings& settings,
                                  RgbColor linearOutput);

// CPU reference for the Metal shader's scene-linear color path. Offline
// export uses this to build a high-precision 3D LUT, while tests use it as the
// parity oracle for known S-Log3/HLG values.
RgbColor TransformColorForOutput(const ColorManagementSettings& settings,
                                 RgbColor signal);

// The project output remains authoritative for export. On-screen editing
// monitors use a display-referred Rec.709 view so an HDR delivery transform
// cannot be interpreted as EDR by the desktop compositor a second time.
ColorManagementSettings ColorManagementForSdrPreview(
    const ColorManagementSettings& settings);

// Resolves the document's readable legacy names through the pinned OCIO
// config and samples the processors into deterministic 65^3 LUTs. The pair is
// used by Metal; the combined .cube is consumed by FFmpeg during export.
bool BuildOpenColorIoLuts(const ColorManagementSettings& settings,
                          OpenColorIoLutPair& output, std::string& error);
// G3 -- docs/roadmap/GRADE-2026-09.md. The pair's input half alone: what a
// frame whose media declares its own input needs beside the project's pair.
bool BuildOpenColorIoInputLut(const ColorManagementSettings& settings,
                              OpenColorIoLut3D& output, std::string& error);
bool BuildOpenColorIoCube(const ColorManagementSettings& settings,
                          std::string& output, std::string& error);

struct ResolvedColorGrade;

// F1.3 -- ROADMAP.md, linear composite (18 September 2026). ITW5 gave each
// managed clip its whole path, display included, before compositing: fades
// and translucent clips then mixed display-encoded signals. The export now
// does what Renderer.mm does with its float working texture: every clip is
// taken to the scene-linear working space, grade included, the clips are
// composited there, and the display transform runs once on the result.
//
// One source signal (full-range RGB, 0..1) in the scene-linear working
// space, grade included: ACEScg when grading in ACEScct, otherwise the
// linear working space. The same order as fragment_working. The export's
// per-clip cube samples this function; tests use it as their reference.
bool OpenColorIoWorkingColor(const ColorManagementSettings& settings,
                             const ResolvedColorGrade& grade, RgbColor signal,
                             RgbColor& working, std::string& error);

// One scene-linear working value as a display signal, as the monitor's
// output pass computes it: ACEScct encoding when grading in ACEScct, the
// LUT coordinates clamped to 0..1, then the display transform.
bool OpenColorIoDisplayColor(const ColorManagementSettings& settings,
                             RgbColor working, RgbColor& display,
                             std::string& error);

// G1 -- docs/roadmap/GRADE-2026-09.md. The two functions above, held open
// for a caller that transforms a whole picture: each of them builds its OCIO
// processors on every call, which a test oracle can afford and a measured
// picture of a hundred thousand pixels cannot. The same per-colour paths run
// behind both, so a measurement reads the values the export's cubes sample.
// The display half is split where a sequence look declared on the working
// log values goes (LookLutStage::WorkingLog): between the LUT coordinates
// and the display transform, as the export's shaper, look and display cube
// are laid.
struct OpenColorIoColorPathState;
struct OpenColorIoColorPath {
    std::shared_ptr<const OpenColorIoColorPathState> state;
};
bool MakeOpenColorIoColorPath(const ColorManagementSettings& settings,
                              OpenColorIoColorPath& path, std::string& error);
// OpenColorIoWorkingColor's value. A non-finite result is the caller's to
// refuse, as OpenColorIoWorkingColor refuses it.
std::array<float, 3> OpenColorIoPathWorking(const OpenColorIoColorPath& path,
                                            const ResolvedColorGrade& grade,
                                            const std::array<float, 3>& signal);
// What the display transform reads for a working value: the ACEScct
// encoding when grading in ACEScct, clamped to the LUT's 0..1 domain.
std::array<float, 3> OpenColorIoPathCoordinates(
    const OpenColorIoColorPath& path, const std::array<float, 3>& working);
// The display signal for those coordinates, clamped to 0..1. Coordinates
// then display is OpenColorIoDisplayColor's value.
std::array<float, 3> OpenColorIoPathDisplay(
    const OpenColorIoColorPath& path, const std::array<float, 3>& coordinates);

// OpenColorIoWorkingColor sampled on the 65^3 grid of source signals, as a
// .cube whose values stay unclamped: the export reads it in float, where a
// highlight above 1.0 survives until the display transform.
bool BuildOpenColorIoWorkingCube(const ColorManagementSettings& settings,
                                 const ResolvedColorGrade& grade,
                                 std::string& output, std::string& error);

// MOTION-2026-09 M4 -- ROADMAP.md. A motion composition is drawn by Core
// Graphics in display-referred sRGB: a hex colour is the code value the
// author wants to see. Taken to the working space by the inverse of the
// SDR display view, it comes back out of the display transform as that
// code value, where decoding it as a camera Rec.709 signal would send it
// through the tone curve of the view and dim a white title (0.82 measured
// for code value 1.0).
//
// BH1 -- docs/roadmap/BLANC-HLG-2026-10.md. Through the SDR view, an HLG
// export put a title's white below the peak (0.93 of the signal, some 640
// of 1000 nits, measured on PRJH on 15 October 2026), where DaVinci
// Resolve puts it at the peak. For an HLG delivery a graphic is now placed
// in the delivery itself: its code value is its HLG signal, primaries
// converted from Rec.709 to Rec.2020 (GraphicsCodeToHlgSignal), taken to the
// working space by the inverse of the HLG view. White comes out at the
// view's peak (0.994: the ACES HDR view saturates just below 1), a neutral
// grey keeps its value, a colour keeps its look on an HLG display. An SDR
// delivery is unchanged.
//
// One display-referred signal (full-range RGB, 0..1) in the scene-linear
// working space, as OpenColorIoWorkingColor gives a camera signal.
bool OpenColorIoDisplayReferredWorkingColor(
    const ColorManagementSettings& settings, RgbColor signal, RgbColor& working,
    std::string& error);

// BH1 -- whether display-referred layers are placed in an HLG delivery
// rather than through the SDR view.
bool GraphicsPlacedInHlg(const ColorManagementSettings& settings);

// BH1 -- a graphic's code value (Rec.709 primaries) as the HLG signal it is
// shown at (Rec.2020 primaries), and back. Each channel goes through the HLG
// transfer to scene light, the primaries are converted there, and the result
// is encoded again: a neutral passes unchanged. Values are bounded to
// [0, 1]; a Rec.2020 colour outside Rec.709 comes back clipped.
RgbColor GraphicsCodeToHlgSignal(RgbColor code);
RgbColor HlgSignalToGraphicsCode(RgbColor signal);

// Visual QA of 19 September 2026 (M4). Mixed in the scene-linear working
// space, a translucent display-referred layer kept its white almost opaque:
// display white sits near 16 in scene-linear, so a white title at 50 % over
// black still mixed to about 8, which the display transform shows as white,
// while a colour with a zero channel let the shot through. Such a layer is
// mixed where its colours were authored instead: the composite under it goes
// out through the SDR display view, the layer is mixed there by its
// coverage, and the result comes back through the inverse the layer itself
// uses (OpenColorIoDisplayReferredWorkingColor). The view is bounded to
// [0, 1], what it shows: a highlight above white mixes as white. A coverage
// of zero leaves the composite untouched, bit for bit; the export and the
// monitor apply this one rule. BH1 -- for an HLG delivery the view is the
// HLG one read back as graphic code values (HlgSignalToGraphicsCode).
bool OpenColorIoCompositeDisplayReferred(
    const ColorManagementSettings& settings, RgbColor below, RgbColor layer,
    double coverage, RgbColor& working, std::string& error);

// BH1 -- the view a display-referred layer is mixed in, as the export's
// shaper and cube (BuildOpenColorIoDisplayCubes's layout) and as the
// monitor's LUT pair: the SDR view, or for an HLG delivery the HLG view
// followed by HlgSignalToGraphicsCode.
bool BuildOpenColorIoGraphicsViewCubes(const ColorManagementSettings& settings,
                                       std::string& shaper, std::string& cube,
                                       std::string& error);
bool BuildOpenColorIoGraphicsViewLuts(const ColorManagementSettings& settings,
                                      OpenColorIoLutPair& output,
                                      std::string& error);

// That function sampled as the export's working-space .cube for a motion
// clip, and as the 3D LUT the monitor applies to a motion frame.
bool BuildOpenColorIoDisplayReferredWorkingCube(
    const ColorManagementSettings& settings, std::string& output,
    std::string& error);
bool BuildOpenColorIoDisplayReferredWorkingLut(
    const ColorManagementSettings& settings, OpenColorIoLut3D& output,
    std::string& error);

// FFmpeg's lut1d accepts at most this many entries.
inline constexpr int32_t kOpenColorIoShaperSize = 65536;

// The export's display side, applied once to the linear composite. Grading
// in ACEScct, `shaper` is a 1D .cube from scene-linear to ACEScct and `cube`
// reads ACEScct; otherwise `shaper` is empty and `cube` reads the linear
// working space. Both clamp their input as the monitor's LUT coordinates
// do, except that the shaper starts at linear zero rather than at ACEScct
// zero (see the .cc).
bool BuildOpenColorIoDisplayCubes(const ColorManagementSettings& settings,
                                  std::string& shaper, std::string& cube,
                                  std::string& error);

// The FFmpeg filters that take a decoded YUV picture through the .cube at
// `lutPath` and back to limited-range YUV in `outputMatrix` ("bt709" or
// "bt2020"), reading the input range and matrix the settings declare. Shared
// by the timeline sheets and read_frame, so both show the same image; the
// export reads the source the same way before its working-space cube.
std::string ColorLutFilter(const ColorManagementSettings& settings,
                           const std::string& lutPath,
                           const std::string& outputMatrix);

// The scale options that read a decoded YCbCr picture with the range and
// matrix the settings declare, each followed by ':', or nothing where they
// are "auto".
std::string ColorInputScaleOptions(const ColorManagementSettings& settings);

// COLOR-T -- what a graph FFmpeg runs must not leave swscale to guess. Under
// "auto" it reads a tagged picture by its tags, as ResolveYuvFullRange and
// ResolveYuvBt2020Matrix do for BT.709 and BT.2020; an untagged one swscale
// reads with the BT.601 matrix and the limited range, where those two read
// BT.709, and full range under an S-Log3 input. An FX30 tags nothing in its
// MP4: under colour management, an export of a clip whose media declares its
// input ("auto", as ingest writes it from the camera's labels) came out
// darker in green on a face than Record, read_frame and the colour measure
// showed it -- 0.011 to 0.019 measured on 4 October 2026, SERIE_Hugo and
// SERIE_Camille of PRJA. This returns `settings` with what that rule
// decides for an untagged picture written in, so that ColorInputScaleOptions
// names it; a declared range or matrix, and a tagged picture, are left as
// they are. `colorRange` and `colorSpace` are a LibraryMedia's probed tags:
// "pc" or "tv", "bt709", "bt2020nc" and so on, "unknown" when the file says
// nothing; an empty tag, which a document that never recorded it holds, is
// left to the graph as before.
ColorManagementSettings ResolveUntaggedColorInput(
    const ColorManagementSettings& settings, const std::string& colorRange,
    const std::string& colorSpace);

// Stable cache key for renderer-owned Metal textures.
std::string OpenColorIoCacheKey(const ColorManagementSettings& settings);
