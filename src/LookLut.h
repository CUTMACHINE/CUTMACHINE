#pragma once

// R6 -- docs/roadmap/REEL-2026-09.md. The look LUT of a sequence: one
// imported .cube laid after every clip's grade and before the output
// transform, once for the whole timeline.
//
// The survey of 19 September measured the need and cut the ticket down to
// it: the first film carried one LUT, the same on all 31 shots, computed on
// the log values of the working space; no grain, no vignette. Effects live
// on the clip (DocumentClip::effects), so that one look was 31 places to
// change. This is the one place. A LUT per clip, grain and a vignette come
// back the day the corpus asks for them.
//
// GUARD RAIL 2 -- a .cube holds floating-point numbers, and a document
// refuses a float literal outright. The boundary is here. The document
// stores a *reference*: the SHA-256 digest of the file's bytes, the name it
// was imported under, and the space its author declared -- never a number
// out of it. The numbers stay in the file, copied into the project package
// under its digest, and are read only at render time: by Metal on the
// monitor, by FFmpeg's lut3d at export, FROM THE SAME FILE. Neither engine
// holds a second spelling of them, and the digest is what says the two read
// the same bytes.
//
// GUARD RAIL 4 -- the author declares the space the LUT expects; the engine
// never guesses one. Under colour management a LUT without a declared space
// is refused, and so is a declared space the managed chain has no stage for.
//
// Portable: no AppKit, no Metal, no FFmpeg (tests/look_lut_tests.cc).

#include "Document.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

// The declared spaces this build knows. Anything else is refused by name
// rather than applied somewhere plausible.
//
// "acescct" is the log encoding of the ACEScct working space -- the values
// the first film's LUT was computed on, and the coordinates the display
// transform already reads (BuildOpenColorIoDisplayCubes). An empty space is
// the composite as the delivery encodes it, which only exists without
// colour management.
inline constexpr const char* kLookLutSpaceWorkingLog = "acescct";

// "rec709" is the delivered Rec.709 SDR signal -- the display code values a
// grading application's "Rec.709 look" is written for, and the commonest LUT
// on the market. Under colour management it has an honest stage only when
// the delivery IS that signal: the output transform's result, before the
// captions. On an HLG delivery there is no such signal to lay it on, and it
// is refused rather than applied to HLG code values it was not written for.
inline constexpr const char* kLookLutSpaceDisplay = "rec709";

// A .cube smaller than 2 has no interval to interpolate over; 65 is the
// edge the project's own OpenColorIO cubes already use, so it is what a
// Metal RGBA32Float 3D texture is known to hold here. Sizes in between
// (17, 33, 64) are the ones a grading application writes.
inline constexpr int32_t kMinLookLutSize = 2;
inline constexpr int32_t kMaxLookLutSize = 65;

// The parsed cube. Red is the fastest-varying coordinate, as a .cube spells
// it and as OpenColorIoLut3D stores it, so the RGBA form uploads straight
// into a Metal 3D texture whose x axis is red.
struct LookLutSamples {
    int32_t size = 0;
    std::vector<float> rgba;
};

// Where the look sits in the chain. Decided by the document alone -- the
// sequence's declared space and the project's colour management -- so the
// monitor and the export cannot reach two different answers.
enum class LookLutStage {
    // The sequence carries no LUT.
    None,
    // No colour management: the composite as the delivery encodes it,
    // clamped to [0, 1], which is where the monitor's displaySignal and the
    // export's last filter both stand.
    Display,
    // Colour management: the ACEScct code values of the working space,
    // between the scene-linear composite and the display transform. Exactly
    // "after each clip's grade, before the output transform".
    WorkingLog,
    // Colour management with a Rec.709 SDR delivery: the delivered signal
    // itself, after the output transform and clamped to [0, 1], as a LUT
    // declared "rec709" expects. The same place Display stands without
    // colour management, reached through the output transform.
    DeliveredDisplay,
};

// The document's whole answer about a sequence's look, refusals included.
// `error` names what is wrong and what would make it right; nothing here
// guesses.
bool ResolveLookLutStage(const SequenceLut& lut,
                         const ColorManagementSettings& color,
                         LookLutStage& stage, std::string& error);

// A 64-character lowercase hexadecimal SHA-256, which is what the document
// is allowed to carry as a reference.
bool IsLookLutDigest(const std::string& digest);

// Where the file lives inside the project package, relative to the
// directory that holds project.cutmachine.json. Content-addressed, so a
// package copied to another machine or another path resolves the same file
// by the same name -- which is the ticket's "reopened from a copy of the
// package at another path, identical picture".
std::string LookLutPackagePath(const std::string& digest);

// Parses a .cube. Refuses, by name, everything the two engines might read
// differently rather than accepting it and hoping:
//   * a 1D LUT (LUT_1D_SIZE), which is not this stage's shape;
//   * a size outside [kMinLookLutSize, kMaxLookLutSize];
//   * a domain other than 0..1, because FFmpeg's lut1d divides by
//     DOMAIN_MAX without subtracting DOMAIN_MIN (measured, see
//     ColorManagement.cc's ShaperText) while a texture lookup would not;
//   * a value outside [0, 1], which FFmpeg keeps in float and clamps in
//     16 bits -- two answers for the same file;
//   * a triplet count that is not size^3, or anything non-finite.
bool ParseLookLutCube(const std::string& text, LookLutSamples& output,
                      std::string& error);

// The reference both engines are measured against: trilinear interpolation
// on the cube's grid, input clamped to [0, 1]. Metal's hardware filtering
// and FFmpeg's `lut3d=interp=trilinear` compute this same value; this
// function is the oracle a portable test can check them against, the place
// TransformColorForOutput occupies for the colour transform. It is never on
// the render path of either engine.
std::array<float, 3> EvaluateLookLut(const LookLutSamples& lut,
                                     std::array<float, 3> color);
