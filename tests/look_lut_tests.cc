// R6 -- docs/roadmap/REEL-2026-09.md. The look LUT's own answers: what a
// .cube is read as, what it is refused for, what it is worth at a colour,
// and where in the chain the document says it belongs.
//
// Portable: no AppKit, no Metal, no FFmpeg. What is measured here is the
// definition the two engines share -- Metal samples the same file on the
// monitor, FFmpeg's lut3d reads it at export -- so a disagreement between
// them is a disagreement with this file, and tests/monitor_export_parity is
// where that is caught on real pixels.

#include "Document.h"
#include "LookLut.h"

#include <array>
#include <cmath>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "TestSupport.h"

namespace {

// This suite's own named test, which TestSupport.h's Test would otherwise
// take the place of for a lambda; kept, with its output.
void Case(const std::string& name, void (*body)()) {
    const int before = failures;
    body();
    if (failures == before) std::cout << "ok: " << name << '\n';
}

// A .cube whose value at (r, g, b) is `sample(r, g, b)`, each coordinate an
// index in [0, size). Written the way a grading application writes one:
// red fastest, three decimals per line.
template <typename Sample>
std::string Cube(int size, const Sample& sample, const std::string& header = "",
                 const std::string& prologue = "") {
    std::ostringstream text;
    text.imbue(std::locale::classic());
    text << "# CUTMACHINE test cube\nTITLE \"test\"\n";
    text << (header.empty() ? "LUT_3D_SIZE " + std::to_string(size) : header)
         << '\n';
    text << prologue;
    for (int blue = 0; blue < size; ++blue) {
        for (int green = 0; green < size; ++green) {
            for (int red = 0; red < size; ++red) {
                const std::array<double, 3> value = sample(red, green, blue);
                text << value[0] << ' ' << value[1] << ' ' << value[2] << '\n';
            }
        }
    }
    return text.str();
}

std::string IdentityCube(int size) {
    const double last = size - 1;
    return Cube(size, [last](int red, int green, int blue) {
        return std::array<double, 3>{red / last, green / last, blue / last};
    });
}

// The inverse on every channel: the one look whose direction cannot be
// mistaken for the identity by an accident of interpolation.
std::string InvertedCube(int size) {
    const double last = size - 1;
    return Cube(size, [last](int red, int green, int blue) {
        return std::array<double, 3>{1.0 - (red / last), 1.0 - (green / last),
                                     1.0 - (blue / last)};
    });
}

bool Near(float value, double expected, double tolerance = 1e-5) {
    return std::abs(value - expected) <= tolerance;
}

SequenceLut Look(const std::string& space) {
    SequenceLut lut;
    lut.digest = std::string(64, 'a');
    lut.name = "look.cube";
    lut.space = space;
    return lut;
}

ColorManagementSettings Managed() {
    ColorManagementSettings settings;
    settings.enabled = true;
    settings.input_gamut = "sony_sgamut3_cine";
    settings.input_transfer = "sony_slog3";
    return settings;
}

}  // namespace

int main() {
    Case("an identity cube reads back and leaves a colour alone", [] {
        LookLutSamples lut;
        std::string error;
        Check(ParseLookLutCube(IdentityCube(17), lut, error),
              "identity cube refused: " + error);
        Check(lut.size == 17, "identity cube size");
        Check(lut.rgba.size() == 17u * 17 * 17 * 4, "identity cube samples");
        for (const std::array<float, 3> colour :
             {std::array<float, 3>{0.0f, 0.0f, 0.0f},
              std::array<float, 3>{0.25f, 0.5f, 0.75f},
              std::array<float, 3>{1.0f, 1.0f, 1.0f}}) {
            const std::array<float, 3> shown = EvaluateLookLut(lut, colour);
            Check(Near(shown[0], colour[0]) && Near(shown[1], colour[1]) &&
                      Near(shown[2], colour[2]),
                  "identity moved a colour");
        }
    });

    Case("a cube is applied, and in the direction it is written", [] {
        LookLutSamples lut;
        std::string error;
        Check(ParseLookLutCube(InvertedCube(2), lut, error),
              "inverted cube refused: " + error);
        const std::array<float, 3> shown =
            EvaluateLookLut(lut, {0.25f, 0.5f, 1.0f});
        Check(
            Near(shown[0], 0.75) && Near(shown[1], 0.5) && Near(shown[2], 0.0),
            "an inverted cube did not invert");
    });

    Case("a value outside the cube is clamped to its domain, not wrapped", [] {
        LookLutSamples lut;
        std::string error;
        Check(ParseLookLutCube(InvertedCube(2), lut, error), error);
        const std::array<float, 3> low =
            EvaluateLookLut(lut, {-2.0f, 0.0f, 0.0f});
        const std::array<float, 3> high =
            EvaluateLookLut(lut, {3.0f, 1.0f, 1.0f});
        Check(Near(low[0], 1.0), "a negative input did not clamp to 0");
        Check(Near(high[0], 0.0), "an input above 1 did not clamp to 1");
    });

    Case("interpolation between samples is trilinear", [] {
        // A cube of two samples per side holding the identity: every value
        // between them is the linear mix, which is what a texture lookup
        // and FFmpeg's interp=trilinear both compute.
        LookLutSamples lut;
        std::string error;
        Check(ParseLookLutCube(IdentityCube(2), lut, error), error);
        const std::array<float, 3> shown =
            EvaluateLookLut(lut, {0.3f, 0.7f, 0.1f});
        Check(Near(shown[0], 0.3, 1e-6) && Near(shown[1], 0.7, 1e-6) &&
                  Near(shown[2], 0.1, 1e-6),
              "a two-sample identity is not the identity between its "
              "corners");
    });

    Case("what a .cube is refused for", [] {
        const auto refused = [](const std::string& text,
                                const std::string& what) {
            LookLutSamples lut;
            std::string error;
            const bool accepted = ParseLookLutCube(text, lut, error);
            Check(!accepted, "accepted a .cube that " + what);
            Check(accepted || !error.empty(),
                  "refusal without a reason: " + what);
        };
        refused("LUT_1D_SIZE 4\n0 0 0\n1 1 1\n", "is a 1D LUT");
        refused(IdentityCube(2) + "0.5 0.5 0.5\n", "holds too many values");
        refused("LUT_3D_SIZE 2\n0 0 0\n1 1 1\n", "holds too few values");
        refused(Cube(
                    2,
                    [](int, int, int) {
                        return std::array<double, 3>{0.0, 0.0, 0.0};
                    },
                    "LUT_3D_SIZE 1"),
                "declares a size of 1");
        refused(Cube(
                    2,
                    [](int, int, int) {
                        return std::array<double, 3>{0.0, 0.0, 0.0};
                    },
                    "LUT_3D_SIZE 66"),
                "declares a size of 66");
        refused(Cube(2,
                     [](int red, int, int) {
                         return std::array<double, 3>{red * 2.0, 0.0, 0.0};
                     }),
                "holds a value above 1");
        refused(Cube(2,
                     [](int red, int, int) {
                         return std::array<double, 3>{-1.0 * red, 0.0, 0.0};
                     }),
                "holds a negative value");
        refused(Cube(
                    2,
                    [](int, int, int) {
                        return std::array<double, 3>{0.0, 0.0, 0.0};
                    },
                    "", "DOMAIN_MAX 2.0 2.0 2.0\n"),
                "declares a domain other than 0..1");
        refused(Cube(
                    2,
                    [](int, int, int) {
                        return std::array<double, 3>{0.0, 0.0, 0.0};
                    },
                    "", "DOMAIN_MIN -0.1 -0.1 -0.1\n"),
                "declares a negative domain minimum");
        refused("# nothing but a comment\n", "declares no size");
        refused("LUT_3D_SIZE 2\nSHAPER_LUT 4\n0 0 0\n",
                "carries a keyword "
                "this build does not know");
        refused("0 0 0\nLUT_3D_SIZE 2\n", "puts a value before its size");
    });

    Case("a digest is a SHA-256 and the package path is content-addressed", [] {
        Check(IsLookLutDigest(std::string(64, 'a')), "64 hex refused");
        Check(!IsLookLutDigest(std::string(63, 'a')), "63 hex accepted");
        Check(!IsLookLutDigest(std::string(64, 'A')),
              "uppercase accepted: the digest has one spelling");
        Check(!IsLookLutDigest(std::string(64, 'z')), "non-hex accepted");
        Check(!IsLookLutDigest(""), "an empty digest accepted");
        Check(LookLutPackagePath(std::string(64, 'a')) ==
                  "Luts/" + std::string(64, 'a') + ".cube",
              "the package path moved");
    });

    Case("where the document says the look belongs", [] {
        LookLutStage stage = LookLutStage::None;
        std::string error;
        ColorManagementSettings unmanaged;

        Check(ResolveLookLutStage(SequenceLut{}, unmanaged, stage, error) &&
                  stage == LookLutStage::None,
              "a sequence without a LUT has a stage: " + error);

        Check(
            ResolveLookLutStage(Look(""), unmanaged, stage, error) &&
                stage == LookLutStage::Display,
            "an undeclared LUT without colour management is refused: " + error);

        // Guard rail 4: the author declares, the engine refuses rather than
        // supposes. Under colour management there is no honest stage for a
        // LUT whose space nobody named.
        Check(!ResolveLookLutStage(Look(""), Managed(), stage, error),
              "an undeclared LUT is accepted under colour management");
        Check(error.find("declare") != std::string::npos,
              "the refusal does not say to declare a space: " + error);

        Check(ResolveLookLutStage(Look("acescct"), Managed(), stage, error) &&
                  stage == LookLutStage::WorkingLog,
              "an acescct LUT is refused under colour management: " + error);

        Check(!ResolveLookLutStage(Look("acescct"), unmanaged, stage, error),
              "an acescct LUT is accepted without colour management, where "
              "no such value exists");

        Check(!ResolveLookLutStage(Look("p3-d65"), Managed(), stage, error),
              "a space this build has no stage for is accepted");

        // R6 -- a display LUT under colour management lands on the
        // delivered signal, and only when that signal is Rec.709 SDR.
        Check(ResolveLookLutStage(Look("rec709"), Managed(), stage, error) &&
                  stage == LookLutStage::DeliveredDisplay,
              "a rec709 LUT is refused on a Rec.709 delivery: " + error);
        ColorManagementSettings hlg = Managed();
        hlg.output_gamut = "rec2020";
        hlg.output_transfer = "hlg";
        Check(!ResolveLookLutStage(Look("rec709"), hlg, stage, error),
              "a rec709 LUT is accepted on an HLG delivery, which has no "
              "Rec.709 signal to lay it on");
        Check(error.find("hlg") != std::string::npos,
              "the refusal does not name the delivery: " + error);
        Check(!ResolveLookLutStage(Look("rec709"), unmanaged, stage, error),
              "a rec709 LUT is accepted without colour management, where a "
              "space is left out");

        ColorManagementSettings linearWorking = Managed();
        linearWorking.working_gamut = "rec2020";
        Check(
            !ResolveLookLutStage(Look("acescct"), linearWorking, stage, error),
            "an acescct LUT is accepted with a linear working space, where "
            "there is no log stage to lay it at");

        SequenceLut malformed = Look("");
        malformed.digest = "not-a-digest";
        Check(!ResolveLookLutStage(malformed, unmanaged, stage, error),
              "a malformed digest is accepted");
    });

    Case("a document carries the look, or is refused by name", [] {
        Document document;
        document.sequence.tracks = {{GenerateUlid(), "video", 0, {}}};
        std::string error;
        Check(document.Validate(error), "empty document invalid: " + error);

        document.sequence.look_lut = Look("");
        Check(document.Validate(error),
              "an undeclared look without colour management is refused: " +
                  error);

        document.color_management = Managed();
        Check(!document.Validate(error),
              "switching colour management on left an undeclared look "
              "valid");

        document.sequence.look_lut.space = "acescct";
        Check(document.Validate(error), "a declared look is refused: " + error);

        document.sequence.look_lut.name = "../escape.cube";
        Check(!document.Validate(error),
              "a look name holding a path is accepted");

        document.sequence.look_lut = SequenceLut{};
        document.sequence.look_lut.space = "acescct";
        Check(!document.Validate(error),
              "a look with a space and no digest is accepted");
    });

    Case("only a look makes a document version 14", [] {
        DocumentSequence sequence;
        Check(!HoldsSequenceLut(sequence), "an empty sequence holds a look");
        Check(DocumentFormatVersion(false, false, false, false, false, false,
                                    false) == 7,
              "a plain document is no longer version 7");
        Check(DocumentFormatVersion(false, false, false, false, false, false,
                                    true) == kDocumentVersionWithSequenceLut,
              "a look does not ask for version 14");
        sequence.look_lut.space = "acescct";
        Check(HoldsSequenceLut(sequence),
              "a look spelled without a digest is not counted: an older "
              "reader would drop the key");
    });

    if (failures == 0) std::cout << "PASS: look LUT\n";
    return failures == 0 ? 0 : 1;
}
