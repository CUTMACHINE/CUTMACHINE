// CH5 -- docs/roadmap/CHARTE-2026-11.md. What the Charte panel renders: the
// three sizes, a piece's own name for each (never assumed), the frame the
// preview is taken at, the placeholder of a missing logo, and the request
// once the charter is applied as add_motion_template applies it. The
// request is evaluated as the executable's renderer evaluates it, on fake
// metrics. Portable: no framework, a neutral charter.

#include "CharterPreview.h"

#include "Json.h"
#include "MotionComposition.h"
#include "MotionEvaluate.h"
#include "MotionFrameRenderer.h"
#include "MotionTemplates.h"
#include "TestSupport.h"
#include "motion_fake_measurer.h"

#include <algorithm>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

using mcp_json::Value;
using ui::charter::PiecePreview;
using ui::charter::PreviewFrame;
using ui::charter::PreviewFrames;

const MotionTemplateInfo* Find(const std::vector<MotionTemplateInfo>& pieces,
                               const std::string& name) {
    for (const MotionTemplateInfo& piece : pieces) {
        if (piece.name == name) {
            return &piece;
        }
    }
    return nullptr;
}

MotionKitInfo Kit(const std::string& json) {
    MotionKitInfo kit;
    std::string error;
    Check(ParseMotionKit("marque", json, kit, error),
          "the test charter reads: " + error);
    return kit;
}

std::string Text(const Value& values, const std::string& name) {
    const Value* value = values.Find(name);
    return value != nullptr ? value->Dump() : std::string("<none>");
}

// The request as the executable's renderer evaluates it (MotionCli.cc):
// the format resolved, the frame evaluated, then drawn at its scale.
bool Evaluate(const MotionFrameRequest& request, MotionFrame& frame) {
    MotionComposition resolved;
    Value props;
    MotionError error;
    FakeMotionMeasurer measurer;
    const bool ok = request.composition != nullptr &&
                    ResolveMotionVariant(*request.composition, request.variant,
                                         request.format, request.props,
                                         resolved, props, error) &&
                    EvaluateMotionFrame(resolved, props, request.frame,
                                        measurer, frame, error);
    Check(ok, "the request evaluates: " + error.code + " -- " + error.message);
    if (ok) {
        ScaleMotionFrame(frame, request.scale);
    }
    return ok;
}

const auto kEveryImage = [](const std::string&) { return true; };
const auto kNoImage = [](const std::string&) { return false; };

void CheckFrames() {
    const std::vector<PreviewFrame>& frames = PreviewFrames();
    Check(frames.size() == 3, "three previews");
    if (frames.size() != 3) {
        return;
    }
    Check(frames[0].width == 1080 && frames[0].height == 1920 &&
              frames[0].name == "9x16" && frames[0].label == "9:16",
          "the first preview is 9:16, 1080x1920");
    Check(frames[1].width == 1920 && frames[1].height == 1080 &&
              frames[1].name == "16x9" && frames[1].label == "16:9",
          "the second is 16:9, 1920x1080");
    Check(frames[2].width == 1080 && frames[2].height == 1080 &&
              frames[2].name == "1x1" && frames[2].label == "1:1",
          "the third is 1:1, 1080x1080");
}

void CheckFormatBySize(const std::vector<MotionTemplateInfo>& shipped) {
    for (const char* name : {"logo", "nom", "carton"}) {
        const MotionTemplateInfo* piece = Find(shipped, name);
        Check(piece != nullptr, std::string(name) + " is shipped");
        if (piece == nullptr) {
            continue;
        }
        for (const PreviewFrame& frame : PreviewFrames()) {
            Check(ui::charter::PieceFormatFor(piece->definition, frame.width,
                                              frame.height) == frame.name,
                  std::string(name) + " names its " + frame.label +
                      " format as the usual name");
        }
        Check(ui::charter::PieceFormatFor(piece->definition, 720, 1280).empty(),
              std::string(name) +
                  ": a size it does not declare has no format, even of the "
                  "same aspect");
    }

    // A library piece names its formats itself.
    MotionTemplateInfo badge;
    std::string error;
    Check(ParseMotionPiece(
              "badge",
              R"({"format":"motion-piece.v1","description":"Un badge.",)"
              R"("composition":{"width":1080,"height":1920,"fps":25,)"
              R"("duration":41,"props":{"accent":{"type":"color",)"
              R"("default":"#202020"}},"formats":{"portrait":{"width":1080,)"
              R"("height":1920},"carre":{"width":1080,"height":1080}},)"
              R"("root":{"type":"fill","children":[{"type":"box","id":"b",)"
              R"("style":{"width":100,"height":100,"backgroundColor":)"
              R"({"prop":"accent"}}}]}}})",
              badge, error),
          "the custom piece reads: " + error);
    Check(ui::charter::PieceFormatFor(badge.definition, 1080, 1920) ==
                  "portrait" &&
              ui::charter::PieceFormatFor(badge.definition, 1080, 1080) ==
                  "carre" &&
              ui::charter::PieceFormatFor(badge.definition, 1920, 1080).empty(),
          "a custom piece's formats are found by their size, by their own "
          "names");
    Check(ui::charter::RestFrame(badge.definition) == 20,
          "the rest frame of 41 frames is 20, rounded down");

    const MotionKitInfo kit =
        Kit(R"({"format":"brand-kit.v1","description":"Charte d’exemple.",)"
            R"("props":{"accent":"#1D6FA3"}})");
    PiecePreview preview;
    Check(ui::charter::BuildPieceRequest(kit, badge, PreviewFrames()[0], "",
                                         kEveryImage, preview, error) &&
              preview.format == "portrait",
          "the request of a custom piece names its own format: " + error);
    error.clear();
    Check(!ui::charter::BuildPieceRequest(kit, badge, PreviewFrames()[1], "",
                                          kEveryImage, preview, error) &&
              error.find("16:9") != std::string::npos &&
              error.find("badge") != std::string::npos,
          "no format of that size: refused, in French, naming the size: " +
              error);
}

void CheckRestFrame(const std::vector<MotionTemplateInfo>& shipped) {
    const MotionTemplateInfo* logo = Find(shipped, "logo");
    const MotionTemplateInfo* nom = Find(shipped, "nom");
    const MotionTemplateInfo* carton = Find(shipped, "carton");
    if (logo == nullptr || nom == nullptr || carton == nullptr) {
        Check(false, "logo, nom and carton are shipped");
        return;
    }
    Check(ui::charter::RestFrame(logo->definition) == 125 &&
              ui::charter::RestFrame(nom->definition) == 50 &&
              ui::charter::RestFrame(carton->definition) == 37,
          "each piece rests at half its duration");
    Check(ui::charter::RestFrame(Value::MakeObject()) == 0,
          "no duration: frame 0");

    // At the rest frame every piece is shown whole; at its first frame
    // the logo is not shown at all.
    const MotionKitInfo kit =
        Kit(R"({"format":"brand-kit.v1","description":"Charte d’exemple.",)"
            R"("props":{"police":"Exemple Sans"}})");
    PiecePreview preview;
    std::string error;
    Check(
        ui::charter::BuildPieceRequest(kit, *logo, PreviewFrames()[0],
                                       "logo.png", kEveryImage, preview, error),
        "the logo's request builds: " + error);
    MotionFrame frame;
    if (Evaluate(ui::charter::PieceFrameRequest(preview, 1.0), frame)) {
        Check(frame.root.children.size() == 1 &&
                  frame.root.children[0].children.size() == 1 &&
                  frame.root.children[0].children[0].opacity == 1.0,
              "at the rest frame the logo is at full opacity");
    }
    MotionFrameRequest first = ui::charter::PieceFrameRequest(preview, 1.0);
    first.frame = 0;
    if (Evaluate(first, frame)) {
        Check(frame.root.children[0].children[0].opacity == 0.0,
              "at frame 0 it has not faded in");
    }
}

void CheckPlaceholder(const std::vector<MotionTemplateInfo>& shipped) {
    const MotionTemplateInfo* logo = Find(shipped, "logo");
    const MotionTemplateInfo* carton = Find(shipped, "carton");
    if (logo == nullptr || carton == nullptr) {
        Check(false, "logo and carton are shipped");
        return;
    }
    const MotionKitInfo neutral =
        Kit(R"({"format":"brand-kit.v1","description":"Charte d’exemple.",)"
            R"("props":{"police":"Exemple Sans"}})");
    PiecePreview preview;
    std::string error;
    Check(ui::charter::BuildPieceRequest(neutral, *logo, PreviewFrames()[0],
                                         "/neutre/substitut.png", kNoImage,
                                         preview, error),
          "a missing logo still builds a request: " + error);
    Check(preview.substituted ==
                  std::vector<std::pair<std::string, std::string>>{
                      {"logo", "logo.png"}} &&
              Text(preview.props, "logo") == R"("/neutre/substitut.png")",
          "the piece's own logo, missing, is replaced and said: " +
              preview.props.Dump());

    // The charter's logo file: replaced when this machine lacks it, kept
    // when it has it.
    const MotionKitInfo withLogo =
        Kit(R"({"format":"brand-kit.v1","description":"Charte d’exemple.",)"
            R"("pieces":{"logo":{"logo":"/kits/marque/logo.png"}}})");
    const auto onlyCharterLogo = [](const std::string& path) {
        return path == "/kits/marque/logo.png";
    };
    Check(ui::charter::BuildPieceRequest(withLogo, *logo, PreviewFrames()[1],
                                         "/neutre/substitut.png",
                                         onlyCharterLogo, preview, error) &&
              preview.substituted.empty() && preview.props.IsNull(),
          "the charter's logo, present, is rendered as it is: " + error);
    Check(ui::charter::BuildPieceRequest(withLogo, *logo, PreviewFrames()[1],
                                         "/neutre/substitut.png", kNoImage,
                                         preview, error) &&
              preview.substituted ==
                  std::vector<std::pair<std::string, std::string>>{
                      {"logo", "/kits/marque/logo.png"}},
          "the charter's logo, missing, is replaced and named with its "
          "path: " +
              error);
    const MotionFrameRequest request =
        ui::charter::PieceFrameRequest(preview, 1.0);
    Check(Text(request.props, "logo") == R"("/neutre/substitut.png")",
          "the request carries the placeholder");

    // A piece without an image substitutes nothing.
    Check(ui::charter::BuildPieceRequest(neutral, *carton, PreviewFrames()[2],
                                         "/neutre/substitut.png", kNoImage,
                                         preview, error) &&
              preview.substituted.empty() && preview.props.IsNull(),
          "a card has nothing to substitute: " + error);
}

void CheckRequest(const std::vector<MotionTemplateInfo>& shipped) {
    const MotionTemplateInfo* nom = Find(shipped, "nom");
    const MotionTemplateInfo* logo = Find(shipped, "logo");
    const MotionTemplateInfo* carton = Find(shipped, "carton");
    if (nom == nullptr || logo == nullptr || carton == nullptr) {
        Check(false, "nom, logo and carton are shipped");
        return;
    }
    const MotionKitInfo kit =
        Kit(R"({"format":"brand-kit.v1","description":"Charte d’exemple.",)"
            R"("props":{"accent":"#1D6FA3","police":"Exemple Sans"},)"
            R"("pieces":{"logo":{"marge_droite":10}},)"
            R"("withdrawn":{"carton":"pas besoin"}})");
    PiecePreview preview;
    std::string error;
    Check(ui::charter::BuildPieceRequest(kit, *nom, PreviewFrames()[1], "",
                                         kEveryImage, preview, error),
          "the name's request builds: " + error);
    Check(preview.piece == "nom" && preview.format == "16x9" &&
              preview.frame.label == "16:9" && preview.rest_frame == 50,
          "the request names the piece, its format and its rest frame");
    Check(std::find(preview.application.applied.begin(),
                    preview.application.applied.end(),
                    "accent") != preview.application.applied.end(),
          "the charter's accent is applied to the name");
    const Value values = ui::charter::PieceValues(preview);
    Check(Text(values, "accent") == R"("#1D6FA3")" &&
              Text(values, "police") == R"("Exemple Sans")",
          "the charter's colour and font reach the props: " + values.Dump());
    Check(Text(values, "corps") == "56" && Text(values, "hauteur") == "12",
          "the format's own values hold in its preview: " + values.Dump());
    Check(values.Find("texte_group") == nullptr,
          "a group has no value: " + values.Dump());

    const MotionFrameRequest request =
        ui::charter::PieceFrameRequest(preview, 0.25);
    Check(request.composition == &preview.composition &&
              request.format == "16x9" && request.variant.empty() &&
              request.frame == 50 && request.scale == 0.25 &&
              request.props.IsNull() && request.raster,
          "the request points at the preview's composition, its format, "
          "its rest frame and the scale");
    MotionFrame frame;
    if (Evaluate(request, frame)) {
        Check(frame.width == 480 && frame.height == 270,
              "the frame is drawn at a quarter of 1920x1080");
    }

    // The charter's value for the logo holds in every format it applies to.
    Check(
        ui::charter::BuildPieceRequest(kit, *logo, PreviewFrames()[0],
                                       "logo.png", kEveryImage, preview, error),
        "the logo's request builds: " + error);
    Check(Text(ui::charter::PieceValues(preview), "marge_droite") == "10",
          "the charter's margin replaces the 9:16 format's 14");

    error.clear();
    Check(!ui::charter::BuildPieceRequest(kit, *carton, PreviewFrames()[0], "",
                                          kEveryImage, preview, error) &&
              error.find("pas besoin") != std::string::npos &&
              error.find("carton") != std::string::npos,
          "a withdrawn piece is refused with the charter's reason: " + error);
}

}  // namespace

int main() {
    std::vector<MotionTemplateInfo> shipped;
    std::string error;
    Check(MotionTemplateCatalog(shipped, error), "catalog loads: " + error);
    CheckFrames();
    CheckFormatBySize(shipped);
    CheckRestFrame(shipped);
    CheckPlaceholder(shipped);
    CheckRequest(shipped);
    if (failures == 0) std::cout << "charter preview tests passed\n";
    return failures == 0 ? 0 : 1;
}
