// Pure-C++ test for the Media panel's tab content view-model (ROADMAP.md
// F2.3). MediaPanelModel.h has no AppKit dependency -- it links only
// against cutmachine_model (Document.h/Operations.h/Ulid.h) -- so it builds
// and runs on a plain Linux host, the same way tests/project_bin_tests.cc
// does.

#include "CaptionOverlayLayout.h"
#include "EditLog.h"
#include "MediaPanelModel.h"

#include <cmath>
#include <iostream>
#include <set>
#include <string>

#include "TestSupport.h"

namespace {

LibraryMedia MakeMedia(std::string filename, bool hasAudio, Ulid binId = {}) {
    LibraryMedia media;
    media.id = GenerateUlid();
    media.filename = std::move(filename);
    media.codec = "h264";
    media.has_audio = hasAudio;
    media.bin_id = std::move(binId);
    return media;
}

}  // namespace

int main() {
    using namespace ui::media_panel;

    Test("AllTabs lists Media, Audio, Captions, Effects in order", [] {
        const auto& tabs = AllTabs();
        Check(tabs.size() == 4, "expected exactly 4 tabs");
        Check(tabs[0] == Tab::Media, "Media should be first");
        Check(tabs[1] == Tab::Audio, "Audio should be second");
        Check(tabs[2] == Tab::Captions, "Captions should be third");
        Check(tabs[3] == Tab::Effects, "Effects should be fourth");
    });

    Test("TabTitle is non-empty and distinct for every tab", [] {
        std::set<std::string> titles;
        for (Tab tab : AllTabs()) {
            const std::string title = TabTitle(tab);
            Check(!title.empty(), "tab title must not be empty");
            titles.insert(title);
        }
        Check(titles.size() == AllTabs().size(),
              "tab titles must be pairwise distinct");
    });

    Test("CarriesAudio mirrors LibraryMedia::has_audio exactly", [] {
        Check(CarriesAudio(MakeMedia("a.mp4", true)),
              "has_audio=true must read as audio-capable");
        Check(!CarriesAudio(MakeMedia("b.mp4", false)),
              "has_audio=false must not read as audio-capable");
    });

    Test("FilterAudioSources drops silent media and keeps audio-capable ones",
         [] {
             std::vector<LibraryMedia> library{
                 MakeMedia("interview.mp4", true),
                 MakeMedia("broll_silent.mp4", false),
                 MakeMedia("music_bed.mp4", true),
             };
             const auto result =
                 FilterAudioSources(library, /*anyBin=*/true,
                                    /*wantRoot=*/false, /*wantBinId=*/{}, "");
             Check(result.size() == 2, "expected 2 audio-capable entries");
             for (const LibraryMedia* media : result)
                 Check(media->has_audio, "every result must carry audio");
         });

    Test("FilterAudioSources honors bin membership", [] {
        const Ulid binA = GenerateUlid();
        const Ulid binB = GenerateUlid();
        std::vector<LibraryMedia> library{
            MakeMedia("in_a.mp4", true, binA),
            MakeMedia("in_b.mp4", true, binB),
            MakeMedia("in_root.mp4", true, Ulid{}),
        };
        const auto inA = FilterAudioSources(library, false, false, binA, "");
        Check(inA.size() == 1 && inA.front()->filename == "in_a.mp4",
              "bin filter should keep only that bin's audio-capable media");
        const auto inRoot = FilterAudioSources(library, false, true, {}, "");
        Check(inRoot.size() == 1 && inRoot.front()->filename == "in_root.mp4",
              "root filter should keep only unbinned audio-capable media");
        const auto everywhere =
            FilterAudioSources(library, true, false, {}, "");
        Check(everywhere.size() == 3, "anyBin=true should ignore bin_id");
    });

    Test("FilterAudioSources search is a case-insensitive substring match", [] {
        std::vector<LibraryMedia> library{
            MakeMedia("Interview_Take1.mp4", true),
            MakeMedia("broll.mp4", true),
        };
        const auto result =
            FilterAudioSources(library, true, false, {}, "interview");
        Check(result.size() == 1, "expected exactly one match");
        Check(result.front()->filename == "Interview_Take1.mp4",
              "the matching filename should survive the filter");
    });

    Test("FilterAudioSources returns results sorted by filename", [] {
        std::vector<LibraryMedia> library{
            MakeMedia("zebra.mp4", true),
            MakeMedia("apple.mp4", true),
            MakeMedia("mango.mp4", true),
        };
        const auto result = FilterAudioSources(library, true, false, {}, "");
        Check(result.size() == 3, "expected all three entries");
        if (result.size() == 3) {
            Check(result[0]->filename == "apple.mp4",
                  "apple should sort first");
            Check(result[1]->filename == "mango.mp4",
                  "mango should sort second");
            Check(result[2]->filename == "zebra.mp4",
                  "zebra should sort third");
        }
    });

    Test("DirectChildBins exposes only one folder level", [] {
        DocumentBin rootB;
        rootB.name = "B rushes";
        DocumentBin rootA;
        rootA.name = "A interviews";
        DocumentBin nested;
        nested.name = "Nested";
        nested.parent_id = rootA.id;
        const std::vector<DocumentBin> bins{rootB, nested, rootA};

        const auto root = DirectChildBins(bins, {}, "");
        Check(root.size() == 2, "root should contain its two direct bins");
        if (root.size() == 2) {
            Check(root[0]->id == rootA.id, "root bins should sort by name");
            Check(root[1]->id == rootB.id, "root bins should sort by name");
        }
        const auto children = DirectChildBins(bins, rootA.id, "nest");
        Check(children.size() == 1 && children.front()->id == nested.id,
              "a bin should expose its matching direct child");

        const auto tree = BinNavigationTree(bins);
        Check(tree.size() == 3, "navigation should contain every bin");
        if (tree.size() == 3) {
            Check(tree[0].bin->id == rootA.id && tree[0].depth == 0,
                  "first root folder should start at depth zero");
            Check(tree[1].bin->id == nested.id && tree[1].depth == 1,
                  "a child should immediately follow its parent");
            Check(tree[2].bin->id == rootB.id && tree[2].depth == 0,
                  "the next root folder should return to depth zero");
        }
    });

    Test("DescribeCaptionStyle is deterministic and includes every field", [] {
        CaptionStyle style;
        style.font_family = "Helvetica";
        style.font_size = 36;
        style.color = "#00ff00";
        style.position = "top";
        const std::string first = DescribeCaptionStyle(style);
        const std::string second = DescribeCaptionStyle(style);
        Check(first == second, "must be a pure, deterministic function");
        Check(first.find("Helvetica") != std::string::npos,
              "description must mention the font family");
        Check(first.find("36") != std::string::npos,
              "description must mention the font size");
        Check(first.find("top") != std::string::npos,
              "description must mention the position");
        Check(first.find("#00ff00") != std::string::npos,
              "description must mention the color");
    });

    Test("DescribeCaptionStyle names the decoration and custom position", [] {
        CaptionStyle style;
        style.outline_width = 4;
        style.background_color = "#000000@0.5";
        style.vertical_position = {3, 4};
        const std::string description = DescribeCaptionStyle(style);
        Check(description.find("contour 4 #000000") != std::string::npos &&
                  description.find("fond #000000@0.5") != std::string::npos &&
                  description.find("3/4 hauteur") != std::string::npos &&
                  description.find("bottom") == std::string::npos,
              "the decoration and the custom position replace the preset: " +
                  description);
    });

    // ITW4 -- the frame Record draws, which the cartons are laid on (LG5:
    // the cartons themselves are laid out by CaptionLayout.h).
    Test("caption overlay layout follows the frame Record draws", [] {
        namespace overlay = ui::caption_overlay;
        // A 1080x1920 sequence in a 800x600 monitor: 337.5 points wide,
        // centred, 600 high, so one sequence pixel is 0.3125 point.
        const overlay::Layout layout =
            overlay::Compute(1080, 1920, 800.0, 600.0);
        Check(layout.canvas_width == 337.5 && layout.canvas_height == 600.0 &&
                  layout.canvas_x == 231.25 && layout.canvas_y == 0.0 &&
                  layout.scale == 0.3125,
              "the vertical frame is fitted and centred");
        const overlay::Layout wide = overlay::Compute(1920, 1080, 800.0, 600.0);
        Check(wide.canvas_height == 450.0 && wide.canvas_y == 75.0,
              "a wide frame is letterboxed");
        Check(overlay::Compute(0, 1080, 800.0, 600.0).scale == 0.0,
              "an empty sequence lays out nothing");
        // Visual QA of 19 September 2026: zoomed to 100 % on a 2x screen,
        // the frame keeps half a point per pixel whatever the view's size.
        const overlay::Layout zoomed =
            overlay::Compute(1080, 1920, 800.0, 600.0, 0.5);
        Check(zoomed.canvas_width == 540.0 && zoomed.canvas_height == 960.0 &&
                  zoomed.canvas_x == 130.0 && zoomed.canvas_y == -180.0 &&
                  zoomed.scale == 0.5,
              "a zoomed monitor keeps the frame's size");
        const overlay::Layout wider =
            overlay::Compute(1080, 1920, 1000.0, 700.0, 0.5);
        Check(wider.canvas_width == zoomed.canvas_width &&
                  wider.canvas_height == zoomed.canvas_height,
              "resizing a zoomed monitor keeps the frame at the same size");
    });

    Test("SummarizeCaptionStyles preserves caption_styles order", [] {
        DocumentSequence sequence;
        CaptionStyle first;
        first.font_family = "system";
        CaptionStyle second;
        second.font_family = "Georgia";
        sequence.caption_styles = {first, second};
        const auto summary = SummarizeCaptionStyles(sequence);
        Check(summary.size() == 2, "expected one summary per style");
        if (summary.size() == 2) {
            Check(summary[0].style_id == first.id,
                  "first summary must match the first style");
            Check(summary[1].style_id == second.id,
                  "second summary must match the second style");
        }
    });

    Test("SummarizeCaptionStyles counts clips per caption group correctly", [] {
        DocumentSequence sequence;
        CaptionStyle styleA;
        CaptionStyle styleB;
        sequence.caption_styles = {styleA, styleB};

        DocumentTrack captions;
        captions.kind = "caption";
        DocumentClip clipUsingA1;
        clipUsingA1.caption_group_id = styleA.id;
        DocumentClip clipUsingA2;
        clipUsingA2.caption_group_id = styleA.id;
        DocumentClip clipUsingNone;
        captions.clips = {clipUsingA1, clipUsingA2, clipUsingNone};
        // F2.3 -- a picture carrying style B, as set_clip_caption or a
        // project from before #12 leaves it: not a carton.
        DocumentTrack pictures;
        pictures.kind = "video";
        DocumentClip clipUsingB;
        clipUsingB.caption_group_id = styleB.id;
        pictures.clips = {clipUsingB};
        sequence.tracks = {captions, pictures};

        const auto summary = SummarizeCaptionStyles(sequence);
        Check(summary.size() == 2, "expected two style summaries");
        if (summary.size() == 2) {
            Check(summary[0].carton_count == 2 &&
                      summary[0].other_clip_count == 0,
                  "style A dresses exactly 2 cartons");
            Check(summary[1].carton_count == 0 &&
                      summary[1].other_clip_count == 1,
                  "style B is on one picture, which is not a carton");
        }
    });

    Test("SummarizeCaptionStyles returns zero counts for an unused style", [] {
        DocumentSequence sequence;
        CaptionStyle unused;
        sequence.caption_styles = {unused};
        const auto summary = SummarizeCaptionStyles(sequence);
        Check(summary.size() == 1, "expected one summary");
        if (summary.size() == 1)
            Check(summary.front().carton_count == 0 &&
                      summary.front().other_clip_count == 0,
                  "an unreferenced style must report zero clips");
    });

    Test("JoinClipToCaptionStyle builds the exact join operation", [] {
        const Ulid clipId = GenerateUlid();
        const Ulid styleId = GenerateUlid();
        const SetClipCaptionOperation op =
            JoinClipToCaptionStyle(clipId, styleId, "Bonjour");
        Check(op.clip_id == clipId, "clip_id must round-trip");
        Check(op.caption_group_id == styleId,
              "caption_group_id must be the target style");
        Check(op.caption_text == "Bonjour", "caption_text must round-trip");
    });

    Test("ClearClipCaption clears both group and text", [] {
        const Ulid clipId = GenerateUlid();
        const SetClipCaptionOperation op = ClearClipCaption(clipId);
        Check(op.clip_id == clipId, "clip_id must round-trip");
        Check(op.caption_group_id.empty(),
              "clearing must leave caption_group_id empty");
        Check(op.caption_text.empty(),
              "clearing must leave caption_text empty");
    });

    Test("each tab has a stable identifier that reads back", [] {
        for (Tab tab : AllTabs())
            Check(TabFromIdentifier(TabIdentifier(tab)) == tab,
                  std::string("identifier round-trips: ") + TabIdentifier(tab));
        Check(!TabFromIdentifier("inconnu").has_value(),
              "an unknown identifier names no tab");
    });

    // F2.3 rebranched after the visual QA of 19 September 2026: a style
    // dresses the cartons of caption tracks, and keeps their text.
    Test("a style dresses the selected cartons in one undo step", [] {
        Document document;
        document.sources = {{GenerateUlid(), "itw.mov", {25, 1}, {250, 25}}};
        CaptionStyle first;
        CaptionStyle second;
        second.font_size = 64;
        document.sequence.caption_styles = {first, second};
        DocumentClip shot{GenerateUlid(),
                          document.sources[0].id,
                          {0, 25},
                          {100, 25},
                          {0, 25}};
        DocumentClip hello{GenerateUlid(), {}, {0, 1}, {50, 25}, {0, 25}};
        hello.caption_group_id = first.id;
        hello.caption_text = "Bonjour";
        DocumentClip world{GenerateUlid(), {}, {0, 1}, {50, 25}, {50, 25}};
        world.caption_group_id = second.id;
        world.caption_text = "à tous";
        document.sequence.tracks = {
            {GenerateUlid(), "video", 0, {shot}},
            {GenerateUlid(), "caption", 1, {hello, world}}};
        std::string invalid;
        Check(document.Validate(invalid), "fixture: " + invalid);

        const std::vector<Ulid> selection{shot.id, hello.id, world.id};
        const CaptionSelection described =
            DescribeCaptionSelection(document, selection, second.id);
        Check(described.cartons == std::vector<Ulid>{hello.id, world.id} &&
                  described.other_clips == 1 && described.in_style == 1,
              "the picture is told apart from the two cartons, one of which "
              "already has the style");

        const auto batch =
            RestyleCartons(document, described.cartons, second.id);
        Check(batch && batch->operations.size() == 1,
              "only the carton not yet in the style is changed");
        const std::string before = document.SaveToString();
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        Check(batch && log.Apply(document, Operation{*batch}, error, message),
              "the restyle applies: " + message);
        const DocumentClip* restyled = document.FindClip(hello.id);
        Check(restyled && restyled->caption_group_id == second.id &&
                  restyled->caption_text == "Bonjour",
              "the carton takes the style and keeps its text");
        Check(log.AppliedCount() == 1 && log.Undo(document, error, message) &&
                  document.SaveToString() == before,
              "one undo gives the carton its style back");

        const auto plain = RestyleCartons(document, described.cartons, {});
        Check(plain && plain->operations.size() == 2 &&
                  log.Apply(document, Operation{*plain}, error, message) &&
                  document.FindClip(world.id)->caption_text == "à tous" &&
                  document.FindClip(world.id)->caption_group_id.empty(),
              "the default look keeps every carton's text: " + message);
        Check(!RestyleCartons(document, described.cartons, {}).has_value() &&
                  !RestyleCartons(document, {shot.id}, second.id).has_value(),
              "nothing to change, or no carton, gives nothing");
    });

    // D7 -- docs/roadmap/DIALOGUE-2026-10.md.
    Test("a typed carton text is one every reader of a carton takes", [] {
        Check(CartonTextFromTyped("Bonjour") == "Bonjour",
              "a plain text is kept as typed");
        Check(CartonTextFromTyped("  Bonjour \r\n\tà tous  ") ==
                  "Bonjour\nà tous",
              "line ends become one newline, spaces around a line go");
        Check(CartonTextFromTyped("un\n\n \ndeux\n") == "un\ndeux",
              "a blank line would end the cue in an SRT: it goes");
        Check(CartonTextFromTyped(std::string("a\0b\x7f\x01", 5)) == "ab",
              "no control character stays");
        Check(CartonTextFromTyped("Vraiment\xc2\xa0? — l’été") ==
                  "Vraiment\xc2\xa0? — l’été",
              "UTF-8 passes untouched, the no-break space included");
        Check(CartonTextFromTyped(" \n\t\r\n").empty(),
              "only blanks leave nothing");
    });

    Test("a carton's text is rewritten in one edit that keeps its style", [] {
        Document document;
        document.sources = {{GenerateUlid(), "itw.mov", {25, 1}, {250, 25}}};
        CaptionStyle style;
        document.sequence.caption_styles = {style};
        DocumentClip shot{GenerateUlid(),
                          document.sources[0].id,
                          {0, 25},
                          {100, 25},
                          {0, 25}};
        DocumentClip styled{GenerateUlid(), {}, {0, 1}, {50, 25}, {0, 25}};
        styled.caption_group_id = style.id;
        styled.caption_text = "Bonjour a tous";
        DocumentClip plain{GenerateUlid(), {}, {0, 1}, {50, 25}, {50, 25}};
        plain.caption_text = "sans style ";
        DocumentClip kept{GenerateUlid(), {}, {0, 1}, {50, 25}, {0, 25}};
        kept.caption_text = "verrouillé";
        DocumentTrack lockedTrack{GenerateUlid(), "caption", 2, {kept}};
        lockedTrack.locked = true;
        document.sequence.tracks = {
            {GenerateUlid(), "video", 0, {shot}},
            {GenerateUlid(), "caption", 1, {styled, plain}},
            lockedTrack};
        std::string invalid;
        Check(document.Validate(invalid), "fixture: " + invalid);

        Check(EditCartonText(document, shot.id, "x").outcome ==
                      CartonTextOutcome::NotACarton &&
                  EditCartonText(document, GenerateUlid(), "x").outcome ==
                      CartonTextOutcome::NotACarton,
              "a picture, or no clip, has no carton text");
        Check(
            CartonTextAccess(document, kept.id) == CartonTextOutcome::Locked &&
                EditCartonText(document, kept.id, "x").outcome ==
                    CartonTextOutcome::Locked,
            "a carton on a locked track is not asked for");
        Check(CartonTextAccess(document, styled.id) == CartonTextOutcome::Edit,
              "a carton on an open track may be asked for");
        Check(EditCartonText(document, styled.id, " \n ").outcome ==
                  CartonTextOutcome::Empty,
              "a carton is not emptied");
        Check(EditCartonText(document, styled.id, "Bonjour a tous").outcome ==
                      CartonTextOutcome::Unchanged &&
                  EditCartonText(document, styled.id, " Bonjour a tous\n")
                          .outcome == CartonTextOutcome::Unchanged,
              "the same words make no edit");
        Check(EditCartonText(document, plain.id, "sans style ").outcome ==
                  CartonTextOutcome::Unchanged,
              "a text left as it was is not tidied behind one's back");

        const CartonTextEdit edit =
            EditCartonText(document, styled.id, "Bonjour\r\nà tous ");
        Check(edit.outcome == CartonTextOutcome::Edit &&
                  edit.operation.clip_id == styled.id &&
                  edit.operation.caption_group_id == style.id &&
                  edit.operation.caption_text == "Bonjour\nà tous",
              "the edit carries the typed text and the carton's own style");
        const std::string before = document.SaveToString();
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        Check(log.Apply(document, Operation{edit.operation}, error, message),
              "the edit applies: " + message);
        const DocumentClip* rewritten = document.FindClip(styled.id);
        Check(rewritten && rewritten->caption_text == "Bonjour\nà tous" &&
                  rewritten->caption_group_id == style.id &&
                  rewritten->timeline_in == styled.timeline_in &&
                  rewritten->duration == styled.duration,
              "the carton has the new text, its style and its place");
        Check(log.AppliedCount() == 1 && log.Undo(document, error, message) &&
                  document.SaveToString() == before,
              "one undo gives the text back, byte for byte");

        const CartonTextEdit unstyled =
            EditCartonText(document, plain.id, "sans style, corrigé");
        Check(unstyled.outcome == CartonTextOutcome::Edit &&
                  unstyled.operation.caption_group_id.empty(),
              "a carton in the default look stays in it");

        Check(SelectedCartonForText(document, {styled.id}) == styled.id,
              "one selected carton is the one a key opens");
        Check(!SelectedCartonForText(document, {}).has_value() &&
                  !SelectedCartonForText(document, {shot.id}).has_value() &&
                  !SelectedCartonForText(document, {styled.id, plain.id})
                       .has_value(),
              "no clip, a picture, or two cartons open nothing");

        Check(DisplayedCartonText(styled, std::nullopt) == "Bonjour a tous" &&
                  DisplayedCartonText(styled,
                                      CartonTextPreview{plain.id, "autre"}) ==
                      "Bonjour a tous",
              "without a preview of its own, a carton shows its words");
        Check(
            DisplayedCartonText(
                styled, CartonTextPreview{styled.id, " Bonjour\r\nà tous "}) ==
                "Bonjour\nà tous",
            "a carton being typed shows the text it would hold");
        Check(
            DisplayedCartonText(styled, CartonTextPreview{styled.id, " \n"}) ==
                "Bonjour a tous",
            "a field emptied shows the carton's own words");

        // Stored out of order on purpose: the walk follows time.
        DocumentClip third{GenerateUlid(), {}, {0, 1}, {25, 25}, {150, 25}};
        third.caption_text = "trois";
        DocumentClip second{GenerateUlid(), {}, {0, 1}, {25, 25}, {100, 25}};
        second.caption_text = "deux";
        DocumentTrack walked{GenerateUlid(), "caption", 3, {third, second}};
        walked.locked = true;
        document.sequence.tracks.push_back(walked);
        Check(AdjacentCarton(document, styled.id, true) == plain.id &&
                  AdjacentCarton(document, plain.id, false) == styled.id,
              "next and previous follow the track");
        Check(!AdjacentCarton(document, styled.id, false).has_value() &&
                  !AdjacentCarton(document, plain.id, true).has_value(),
              "the walk stops at both ends, and stays on its track");
        Check(AdjacentCarton(document, second.id, true) == third.id &&
                  AdjacentCarton(document, third.id, false) == second.id,
              "the walk follows time, not storage, and reads a locked track");
        Check(!AdjacentCarton(document, shot.id, true).has_value() &&
                  !AdjacentCarton(document, GenerateUlid(), true).has_value(),
              "a picture, or no clip, has no neighbour carton");
    });

    // F2.3 -- a style on pictures only no longer blocks its deletion, and
    // the pictures lose it in the same undo step.
    Test("deleting a style takes it off pictures, never off cartons", [] {
        Document document;
        document.sources = {{GenerateUlid(), "itw.mov", {25, 1}, {250, 25}}};
        CaptionStyle worn;
        CaptionStyle stray;
        CaptionStyle unused;
        document.sequence.caption_styles = {worn, stray, unused};
        DocumentClip shot{GenerateUlid(),
                          document.sources[0].id,
                          {0, 25},
                          {100, 25},
                          {0, 25}};
        shot.caption_group_id = stray.id;
        shot.caption_text = "texte d'avant #12";
        DocumentClip carton{GenerateUlid(), {}, {0, 1}, {50, 25}, {0, 25}};
        carton.caption_group_id = worn.id;
        carton.caption_text = "Bonjour";
        document.sequence.tracks = {{GenerateUlid(), "video", 0, {shot}},
                                    {GenerateUlid(), "caption", 1, {carton}}};
        std::string invalid;
        Check(document.Validate(invalid), "fixture: " + invalid);

        Check(!CaptionStyleRemoval(document, worn.id).has_value(),
              "a style a carton wears is not deleted");
        Check(!CaptionStyleRemoval(document, GenerateUlid()).has_value(),
              "an unknown style gives nothing");
        const auto alone = CaptionStyleRemoval(document, unused.id);
        Check(alone &&
                  std::holds_alternative<RemoveCaptionStyleOperation>(*alone),
              "a style nothing wears is removed alone");

        const auto removal = CaptionStyleRemoval(document, stray.id);
        Check(removal && std::holds_alternative<BatchOperation>(*removal) &&
                  std::get<BatchOperation>(*removal).operations.size() == 2,
              "a style on a picture: the picture loses it, then it goes");
        const std::string before = document.SaveToString();
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        Check(removal && log.Apply(document, *removal, error, message),
              "the removal applies: " + message);
        const DocumentClip* cleared = document.FindClip(shot.id);
        Check(document.FindCaptionStyle(stray.id) == nullptr && cleared &&
                  cleared->caption_group_id.empty() &&
                  cleared->caption_text == "texte d'avant #12",
              "the style is gone, the picture keeps its text");
        Check(log.AppliedCount() == 1 && log.Undo(document, error, message) &&
                  document.SaveToString() == before,
              "one undo restores the style and the picture to the byte");
    });

    Test("the tab sets a style's font, size and colour, nothing else", [] {
        DocumentSequence sequence;
        CaptionStyle style;
        style.outline_width = 3;
        style.background_color = "black@0.5";
        style.color = "white@0.8";
        sequence.caption_styles = {style};
        std::string refusal;

        CaptionStyleLook same{style.font_family, style.font_size, style.color};
        Check(!RestyleCaptionStyle(sequence, style.id, same, refusal) &&
                  refusal.empty(),
              "an unchanged look is no edit and no refusal");

        const std::string red = CaptionColorFromWell(style.color, 255, 0, 16);
        Check(red == "#ff0010@0.8",
              "the well keeps the stored opacity: " + red);
        Check(CaptionColorFromWell("#ffffff", 0, 128, 255) == "#0080ff",
              "an opaque colour stays plain hex");

        CaptionStyleLook look{"Avenir Next", 64, red};
        const auto update =
            RestyleCaptionStyle(sequence, style.id, look, refusal);
        Check(update && update->style.id == style.id &&
                  update->style.font_family == "Avenir Next" &&
                  update->style.font_size == 64 &&
                  update->style.color == "#ff0010@0.8",
              "the three settings change");
        Check(update && update->style.outline_width == 3 &&
                  update->style.background_color == "black@0.5" &&
                  update->style.position == style.position,
              "every other setting is kept");

        Document document;
        document.sequence.caption_styles = {style};
        const std::string before = document.SaveToString();
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        Check(update &&
                  log.Apply(document, Operation{*update}, error, message) &&
                  document.FindCaptionStyle(style.id)->font_size == 64 &&
                  log.Undo(document, error, message) &&
                  document.SaveToString() == before,
              "one edit, one undo, to the byte: " + message);

        look.font_size = kCaptionTabMaximumFontSize + 1;
        Check(!RestyleCaptionStyle(sequence, style.id, look, refusal) &&
                  refusal.find("400") != std::string::npos,
              "a size out of bounds is refused and says the bounds");
        look.font_size = 64;
        look.font_family.clear();
        Check(!RestyleCaptionStyle(sequence, style.id, look, refusal) &&
                  !refusal.empty(),
              "no font is refused");
        look.font_family = "Avenir Next";
        Check(!RestyleCaptionStyle(sequence, GenerateUlid(), look, refusal) &&
                  !refusal.empty(),
              "an unknown style is refused");
    });

    // F2.3 -- the Inspector's style section: the whole look, as Resolve's
    // Track Style, within the engine's own bounds.
    Test(
        "the Inspector sets a style's stroke, shadow, background and "
        "position, within the engine's bounds",
        [] {
            DocumentSequence sequence;
            CaptionStyle style;
            style.font_size = 48;
            sequence.caption_styles = {style};
            std::string refusal;
            Check(!RestyleCaptionStyleWhole(sequence, style, refusal) &&
                      refusal.empty(),
                  "the stored style is no edit and no refusal");

            CaptionStyle wanted = style;
            wanted.outline_width = 4;
            wanted.outline_color = "#102030";
            wanted.shadow_offset = 6;
            wanted.shadow_color = "#ff0000";
            wanted = WithCaptionBackground(wanted, true);
            wanted = WithCaptionPlacement(wanted, CaptionPlacement::Custom, 75);
            const auto update =
                RestyleCaptionStyleWhole(sequence, wanted, refusal);
            Check(update && update->style.outline_width == 4 &&
                      update->style.outline_color == "#102030" &&
                      update->style.shadow_offset == 6 &&
                      update->style.background_color ==
                          kCaptionDefaultBackground &&
                      update->style.background_padding == 12 &&
                      update->style.vertical_position.num == 3 &&
                      update->style.vertical_position.den == 4 &&
                      update->style.font_size == 48,
                  "one operation carries the stroke, the shadow, a black "
                  "box with a quarter of the size as margin, and 3/4 of "
                  "the height: " +
                      refusal);
            Document document;
            document.sequence.caption_styles = {style};
            const std::string before = document.SaveToString();
            EditLog log;
            EditError error = EditError::None;
            std::string message;
            Check(update &&
                      log.Apply(document, Operation{*update}, error, message) &&
                      log.AppliedCount() == 1 &&
                      log.Undo(document, error, message) &&
                      document.SaveToString() == before,
                  "one edit, one undo, to the byte: " + message);

            CaptionStyle beyond = style;
            beyond.outline_width = kCaptionStyleMaxOutlineWidth + 1;
            Check(!RestyleCaptionStyleWhole(sequence, beyond, refusal) &&
                      refusal.find(std::to_string(
                          kCaptionStyleMaxOutlineWidth)) != std::string::npos,
                  "a stroke past the engine's bound is refused, the bound "
                  "said: " +
                      refusal);
            beyond = style;
            beyond.background_padding = kCaptionStyleMaxBackgroundPadding + 1;
            Check(!RestyleCaptionStyleWhole(sequence, beyond, refusal) &&
                      refusal.find(
                          std::to_string(kCaptionStyleMaxBackgroundPadding)) !=
                          std::string::npos,
                  "so is a margin past it");
            beyond = style;
            beyond.shadow_offset = -1;
            Check(!RestyleCaptionStyleWhole(sequence, beyond, refusal) &&
                      !refusal.empty(),
                  "and a negative shadow");
            std::string invalid;
            beyond = style;
            beyond.outline_width = kCaptionStyleMaxOutlineWidth;
            Check(ValidateCaptionStyle(beyond, invalid) &&
                      RestyleCaptionStyleWhole(sequence, beyond, refusal),
                  "the bound itself is the engine's: accepted by both");

            // Placement: presets and the custom height, exact.
            CaptionStyle placed = style;
            Check(CaptionPlacementOf(placed) == CaptionPlacement::Bottom &&
                      CaptionHeightPercent(placed) == 90,
                  "the default is the bottom preset");
            placed = WithCaptionPlacement(placed, CaptionPlacement::Top, 40);
            Check(placed.position == "top" &&
                      placed.vertical_position.num == 0 &&
                      placed.vertical_position.den == 1 &&
                      CaptionPlacementOf(placed) == CaptionPlacement::Top,
                  "a preset forgets the height");
            placed.vertical_position = {2, 3};
            Check(CaptionPlacementOf(placed) == CaptionPlacement::Custom &&
                      CaptionHeightPercent(placed) == 67,
                  "a height set elsewhere reads to the nearest hundredth");
            placed = WithCaptionPlacement(placed, CaptionPlacement::Custom, 0);
            Check(placed.vertical_position.num == 1 &&
                      placed.vertical_position.den == 100,
                  "a height is kept strictly inside the frame");
            placed = WithCaptionPlacement(placed, CaptionPlacement::Center, 0);
            Check(placed.position == "center" &&
                      placed.vertical_position.num == 0,
                  "the centre preset");

            CaptionStyle boxed = style;
            boxed.background_color = "#1a1a8cc0";
            boxed.background_padding = 5;
            boxed = WithCaptionBackground(boxed, true);
            Check(boxed.background_color == "#1a1a8cc0" &&
                      boxed.background_padding == 5,
                  "turning on a box already there keeps it");
            Check(WithCaptionBackground(boxed, false).background_color.empty(),
                  "turned off, there is none");
            Check(CaptionColorFromWell("#1a1a8cc0", 255, 0, 0) == "#ff0000c0",
                  "a box's colour keeps its opacity byte");
        });

    // G3 -- docs/roadmap/GRADE-2026-09.md.
    Test(
        "the media menu's colour inputs are readable, and apply as one step",
        [] {
            for (const ColorInputChoice& choice : ColorInputChoices()) {
                Check(!DeclaresColorInput(choice.input) ||
                          IsSupportedColorInput(choice.input.gamut,
                                                choice.input.transfer),
                      std::string("the config reads ") + choice.label);
            }
            Check(!DeclaresColorInput(ColorInputChoices().front().input),
                  "the first choice is the project's");

            Document document;
            LibraryMedia rush = MakeMedia("C0001.MP4", true);
            rush.path = "C0001.MP4";
            rush.width = 1920;
            rush.height = 1080;
            rush.orientation = "landscape";
            rush.rate = {25, 1};
            rush.duration = {100, 25};
            rush.audio_rate = 48000;
            rush.audio_channels = 2;
            LibraryMedia phone = rush;
            phone.id = GenerateUlid();
            phone.path = phone.filename = "IMG_0001.MOV";
            phone.color_input = {"rec2020", "hlg", "auto", "auto"};
            LibraryMedia voice = rush;
            voice.id = GenerateUlid();
            voice.path = voice.filename = "voice.wav";
            voice.has_video = false;
            voice.width = voice.height = 0;
            voice.orientation = "audio";
            voice.audio_rate = 48000;
            voice.audio_channels = 1;
            document.library = {rush, phone, voice};
            Check(
                ColorInputChoiceOf(rush) == 0 &&
                    std::string(
                        ColorInputChoices()[ColorInputChoiceOf(phone)].label) ==
                        "HLG / Rec.2020",
                "a media shows the choice its declaration is");

            const MediaColorInput log = ColorInputChoices()[1].input;
            const auto batch = BuildSetMediaColorInputs(
                document, {{rush.id, log}, {phone.id, log}, {voice.id, log}});
            Check(batch && std::holds_alternative<BatchOperation>(*batch) &&
                      std::get<BatchOperation>(*batch).operations.size() == 2,
                  "two pictures change in one step; the sound is left out");
            EditLog log2;
            EditError error = EditError::None;
            std::string message;
            Check(batch && log2.Apply(document, *batch, error, message) &&
                      document.library[0].color_input == log &&
                      document.library[1].color_input == log &&
                      !DeclaresColorInput(document.library[2].color_input),
                  "it applies: " + message);
            Check(!BuildSetMediaColorInputs(document, {{rush.id, log}}),
                  "a media already read so is no step");
            const auto single = BuildSetMediaColorInputs(
                document, {{phone.id, ColorInputChoices().front().input}});
            Check(single && std::holds_alternative<SetMediaColorInputOperation>(
                                *single),
                  "one media back to the project is one operation");

            // DEC31, BH2 -- the menu offers the graphic, last, and a media
            // declared one shows it.
            const ColorInputChoice& graphicChoice = ColorInputChoices().back();
            Check(graphicChoice.input.graphics &&
                      std::string(graphicChoice.label) ==
                          "Graphisme (logo, image)",
                  "DEC31: the menu offers the graphic");
            const auto declare = BuildSetMediaColorInputs(
                document, {{rush.id, graphicChoice.input}});
            Check(declare && log2.Apply(document, *declare, error, message) &&
                      document.library[0].color_input.graphics &&
                      ColorInputChoiceOf(document.library[0]) ==
                          ColorInputChoices().size() - 1,
                  "DEC31: a graphic is one step, and shows its choice: " +
                      message);
        });

    // PRD37 -- the QA of 3 October 2026 read "terminée · Th…ov · Complete"
    // under the browser: the engine's own English word for the state.
    Test("the media task line says its state in French, once", [] {
        Check(DescribeMediaTask(nullptr).text == "Tâches média : prêt" &&
                  !DescribeMediaTask(nullptr).shows_progress,
              "no task: ready, and no bar");
        MediaTaskSnapshot task;
        task.label = "Thumbnail 02-fixture.mov";
        task.state = MediaTaskState::Succeeded;
        task.progress = 1.0;
        task.detail = "Complete";
        const MediaTaskLine done = DescribeMediaTask(&task);
        Check(done.text == "terminée · Thumbnail 02-fixture.mov" &&
                  !done.shows_progress && !done.cancellable,
              "a finished task says it once, with no bar: " + done.text);
        task.state = MediaTaskState::Cancelled;
        task.detail = "Cancelled before start";
        Check(DescribeMediaTask(&task).text ==
                  "annulée · Thumbnail 02-fixture.mov",
              "a cancelled one too");
        task.state = MediaTaskState::Failed;
        task.detail = "Failed";
        task.error = "no video stream";
        Check(DescribeMediaTask(&task).text ==
                  "échec · Thumbnail 02-fixture.mov · no video stream",
              "a failure says its error, not the engine's word for it");
        task.state = MediaTaskState::Running;
        task.error.clear();
        task.detail = "Starting";
        task.progress = 0.25;
        const MediaTaskLine starting = DescribeMediaTask(&task);
        Check(starting.text == "en cours · Thumbnail 02-fixture.mov" &&
                  starting.shows_progress && starting.cancellable &&
                  starting.progress == 0.25,
              "a running task has its bar, and the engine's start is not a "
              "detail");
        task.detail = "Extraction de l'image";
        task.progress = 3.0;
        const MediaTaskLine step = DescribeMediaTask(&task);
        Check(step.text ==
                      "en cours · Thumbnail 02-fixture.mov · Extraction "
                      "de l'image" &&
                  step.progress == 1.0,
              "what the task reports of itself shows, its progress held in "
              "range");
        task.state = MediaTaskState::Queued;
        task.label.clear();
        task.detail.clear();
        Check(DescribeMediaTask(&task).text == "en attente",
              "an unnamed task leaves no dangling separator");
        for (MediaTaskState state :
             {MediaTaskState::Queued, MediaTaskState::Running,
              MediaTaskState::Succeeded, MediaTaskState::Failed,
              MediaTaskState::Cancelled}) {
            const std::string label = MediaTaskStateLabel(state);
            Check(label != MediaTaskStateName(state),
                  "the state's label is not the engine's name: " + label);
        }
    });

    Test("the line speaks of the latest task waiting or running", [] {
        std::vector<MediaTaskSnapshot> tasks(3);
        tasks[0].state = MediaTaskState::Running;
        tasks[1].state = MediaTaskState::Succeeded;
        tasks[2].state = MediaTaskState::Cancelled;
        Check(ShownMediaTask(tasks) == &tasks[0],
              "a running task before the finished ones that followed it");
        tasks[0].state = MediaTaskState::Succeeded;
        Check(ShownMediaTask(tasks) == &tasks[2], "all over: the latest one");
        Check(ShownMediaTask({}) == nullptr, "none: none");
    });

    // DEC42 -- docs/roadmap/QA-AGENTIQUE-2026-09.md, cases 123 and 134.
    Test("DEC42: the Inspector says a clip's dips and the sequence's look", [] {
        Document document;
        document.sequence.frame_rate = {25, 1};
        const Ulid first = "01KD0000000000000000000001";
        const Ulid second = "01KD0000000000000000000002";
        const Ulid third = "01KD0000000000000000000003";
        DocumentTransition open;
        open.right_clip_id = first;
        open.type = "dip_to_color";
        open.duration = {40, 25};
        DocumentTransition between = open;
        between.left_clip_id = first;
        between.right_clip_id = second;
        between.color = "#FFFFFF";
        between.duration = {6, 25};
        DocumentTransition close = open;
        close.right_clip_id.clear();
        close.left_clip_id = second;
        close.color = "#bfbfbf";
        close.duration = {1, 25};
        DocumentTransition dissolve = between;
        dissolve.type = "cross_dissolve";
        dissolve.left_clip_id = second;
        dissolve.right_clip_id = third;
        document.sequence.transitions = {open, between, close, dissolve};
        const std::vector<std::string> onFirst =
            ClipDipDescriptions(document, first);
        Check(onFirst.size() == 2 &&
                  onFirst[0] == "Ouverture au noir · 40 images" &&
                  onFirst[1] ==
                      "Fondu vers le plan suivant au blanc · "
                      "6 images",
              "the first clip opens on black and dips to white into "
              "the next: " +
                  (onFirst.empty() ? std::string() : onFirst[0]));
        const std::vector<std::string> onSecond =
            ClipDipDescriptions(document, second);
        Check(onSecond.size() == 2 &&
                  onSecond[0] ==
                      "Fondu depuis le plan précédent au "
                      "blanc · 6 images" &&
                  onSecond[1] == "Fermeture par #bfbfbf · 1 image",
              "the second comes from white and closes on another "
              "colour, spelled as the document spells it");
        Check(ClipDipDescriptions(document, third).empty(),
              "a cross dissolve is no dip");

        Check(SequenceLookDescription(document.sequence) == "Aucun",
              "a sequence without a look says so");
        document.sequence.look_lut = {"abc", "chaud.cube", "acescct"};
        Check(SequenceLookDescription(document.sequence) ==
                  "chaud.cube · espace acescct",
              "a look is named with its space");
        document.sequence.look_lut.space.clear();
        Check(SequenceLookDescription(document.sequence) == "chaud.cube",
              "a look declared without a space is named alone");
    });

    // DEC42 -- case 107a. Record's zoom frames the sequence.
    Test("DEC42: zoomed, an unframed picture fits the sequence frame", [] {
        namespace overlay = ui::caption_overlay;
        // A 4K rush in an HD timeline, Record at 100 %: the frame is drawn
        // 1920 x 1080 and the rush fills it, not twice its size.
        const overlay::Layout zoomed =
            overlay::Compute(1920, 1080, 800.0, 450.0, 1.0);
        const overlay::PictureSize uhd = overlay::FitPictureInFrame(
            3840.0, 2160.0, zoomed.canvas_width, zoomed.canvas_height);
        Check(uhd.width == 1920.0 && uhd.height == 1080.0,
              "a 4K rush covers the zoomed HD frame exactly");
        // A vertical rush in that frame at 200 %: pillarboxed as unzoomed.
        const overlay::PictureSize vertical =
            overlay::FitPictureInFrame(1080.0, 1920.0, 3840.0, 2160.0);
        Check(vertical.height == 2160.0 &&
                  std::abs(vertical.width - 1215.0) < 1e-9,
              "a vertical rush keeps its aspect, as high as the frame");
        const overlay::Layout fitted =
            overlay::Compute(1920, 1080, 800.0, 450.0);
        const overlay::PictureSize same = overlay::FitPictureInFrame(
            3840.0, 2160.0, fitted.canvas_width, fitted.canvas_height);
        Check(same.width == fitted.canvas_width &&
                  same.height == fitted.canvas_height,
              "unzoomed, the same fit as before");
        const overlay::PictureSize none =
            overlay::FitPictureInFrame(0.0, 1080.0, 1920.0, 1080.0);
        Check(none.width == 0.0 && none.height == 0.0,
              "no picture, nothing drawn");
    });

    return failures == 0 ? 0 : 1;
}
