#include "ColorEffects.h"
#include "EditLog.h"
#include "Relink.h"

#include <iostream>

#include "TestSupport.h"

namespace {

Document Fixture() {
    Document document;
    LibraryMedia media;
    media.id = "01K00000000000000000000001";
    media.path = "offline.mov";
    media.filename = "offline.mov";
    media.codec = "h264";
    media.width = 1920;
    media.height = 1080;
    media.rate = {25, 1};
    media.duration = {100, 25};
    media.orientation = "landscape";
    media.has_audio = true;
    media.audio_rate = 48000;
    media.audio_channels = 2;
    media.proxy_path = ".cutmachine/proxies/old.mov";
    media.metadata_complete = true;
    document.library.push_back(media);
    document.sources.push_back(
        {media.id, media.path, media.rate, media.duration});
    DocumentTrack audio;
    audio.id = "01K00000000000000000000002";
    audio.kind = "audio";
    audio.clips.push_back({"01K00000000000000000000003",
                           media.id,
                           {0, 25},
                           {50, 25},
                           {0, 25},
                           false});
    document.sequence.tracks.push_back(audio);
    return document;
}

}  // namespace

int main() {
    Project project = Project::FromDocument(Fixture());
    LibraryMedia replacement = project.rushes[0];
    replacement.path = "/new/camera.mov";
    replacement.filename = "camera.mov";
    replacement.codec = "prores";
    replacement.width = 3840;
    replacement.height = 2160;
    replacement.duration = {125, 25};
    replacement.proxy_path = "must-not-survive";
    std::string error;
    EditError editError = EditError::None;
    ProjectEditLog projectLog;
    const std::string beforeRelink = project.SaveToString();
    Check(projectLog.Apply(
              project,
              ProjectOperation{RelinkProjectMediaOperation{
                  {{replacement.id, replacement, "media/camera.mov"}}}},
              editError, error),
          "compatible replacement applies");
    const std::string afterRelink = project.SaveToString();
    Check(
        project.sources[0].id == replacement.id &&
            project.sources[0].path == "media/camera.mov" &&
            project.rushes[0].path == "media/camera.mov" &&
            project.rushes[0].proxy_path.empty() &&
            project.timelines[0].tracks[0].clips[0].source_id == replacement.id,
        "relink preserves identity and clips while invalidating proxy");
    Check(projectLog.Undo(project, editError, error), "relink undo applies");
    Check(project.SaveToString() == beforeRelink,
          "relink undo restores byte-identical project JSON");
    Check(projectLog.Redo(project, editError, error), "relink redo applies");
    Check(project.SaveToString() == afterRelink,
          "relink redo is deterministic");

    ProjectOperation serializedRelink;
    Check(DeserializeProjectOperation(
              SerializeProjectOperation(projectLog.AppliedEntries().back().op),
              serializedRelink, editError, error),
          "relink operation serializes");
    const auto* reparsedRelink =
        std::get_if<RelinkProjectMediaOperation>(&serializedRelink);
    Check(reparsedRelink && !reparsedRelink->replacements.empty() &&
              reparsedRelink->replacements[0].replacement.has_video,
          "relink serialization preserves picture capability");

    // QC-2026-09 A3 -- the measured level is what lets the CLI leave silent
    // rushes out. An operation read back from its journal used to lose it;
    // undo and redo stayed exact only because the entry also carries the
    // project delta.
    {
        LibraryMedia measured = replacement;
        measured.audio_level_measured = true;
        measured.audio_level = 12345;
        RelinkProjectMediaOperation relink{{{measured.id, measured, "m.mov"}}};
        const std::string json = SerializeProjectOperation(relink);
        ProjectOperation parsed;
        const bool read =
            DeserializeProjectOperation(json, parsed, editError, error);
        const auto* item =
            read ? std::get_if<RelinkProjectMediaOperation>(&parsed) : nullptr;
        Check(item && item->replacements.size() == 1 &&
                  item->replacements[0].replacement.audio_level_measured &&
                  item->replacements[0].replacement.audio_level == 12345,
              "relink serialization preserves the measured audio level");
        Check(read && SerializeProjectOperation(parsed) == json,
              "relink with a measured level serializes canonically");

        LibraryMedia unmeasured = replacement;
        unmeasured.audio_level_measured = false;
        unmeasured.audio_level = 0;
        Check(SerializeProjectOperation(
                  RelinkProjectMediaOperation{
                      {{unmeasured.id, unmeasured, "u.mov"}}})
                      .find("audio_level") == std::string::npos,
              "an unmeasured level adds nothing older journals lack");
    }

    Project shorter = Project::FromDocument(Fixture());
    LibraryMedia shortMedia = shorter.rushes[0];
    shortMedia.duration = {99, 25};
    ProjectEditLog shorterLog;
    Check(!shorterLog.Apply(shorter,
                            ProjectOperation{RelinkProjectMediaOperation{
                                {{shortMedia.id, shortMedia, "short.mov"}}}},
                            editError, error) &&
              shorter.sources[0].path == "offline.mov",
          "short replacement is rejected transactionally");

    Document silent = Fixture();
    LibraryMedia silentMedia = silent.library[0];
    silentMedia.has_audio = false;
    silentMedia.audio_rate = 0;
    silentMedia.audio_channels = 0;
    Check(!ValidateRelinkCandidate(silent, silentMedia.id, silentMedia, error),
          "silent replacement cannot orphan existing audio clips");

    Document cadence = Fixture();
    LibraryMedia differentRate = cadence.library[0];
    differentRate.rate = {24, 1};
    Check(!ValidateRelinkCandidate(cadence, differentRate.id, differentRate,
                                   error),
          "different frame rate is rejected");

    Project batch = Project::FromDocument(Fixture());
    LibraryMedia batchValid = batch.rushes[0];
    batchValid.duration = {125, 25};
    LibraryMedia batchInvalid = batchValid;
    batchInvalid.rate = {24, 1};
    const std::vector<ProjectRelinkItem> replacements = {
        {batchValid.id, batchValid, "valid.mov"},
        {batchInvalid.id, batchInvalid, "invalid.mov"},
    };
    ProjectEditLog batchLog;
    Check(
        !batchLog.Apply(
            batch, ProjectOperation{RelinkProjectMediaOperation{replacements}},
            editError, error) &&
            batch.sources[0].path == "offline.mov",
        "batch relink is atomic when a replacement becomes invalid");

    // COLOR-T -- a relink moves the file, not the reading of its picture.
    // The case of 4 October 2026: a rush ingested in September, with no
    // as-shot balance and the project's input, relinked to its FX30 file,
    // whose probe reads 4000 K and S-Log3 in the camera's metadata.
    {
        Project relinked = Project::FromDocument(Fixture());
        const LibraryMedia original = relinked.rushes[0];
        LibraryMedia probed = original;
        probed.path = "/Volumes/Disque/Prises/C0102.MP4";
        probed.filename = "C0102.MP4";
        probed.codec = "h264";
        probed.width = 3840;
        probed.height = 2160;
        probed.duration = {216, 25};
        probed.white_balance_kelvin = 4000;
        probed.color_input = {"sony_sgamut3_cine", "sony_slog3", "auto",
                              "auto"};
        KeepMediaReading(original, probed);
        Check(probed.white_balance_kelvin == 0 &&
                  !DeclaresColorInput(probed.color_input),
              "a relink leaves an unknown balance unknown and the "
              "project's input in place");
        Check(probed.filename == "C0102.MP4" && probed.width == 3840 &&
                  probed.codec == "h264" &&
                  probed.duration == RationalTime{216, 25},
              "what the file says of itself is still the probe's");

        const DocumentClip clip = relinked.timelines[0].tracks[0].clips[0];
        const int32_t before = ColorGradeNeutralKelvin(ColorGradeContextFor(
            relinked.MakeDocument(relinked.timelines[0].id), clip));
        ProjectEditLog log;
        Check(log.Apply(relinked,
                        ProjectOperation{RelinkProjectMediaOperation{
                            {{original.id, probed, "../Prises/C0102.MP4"}}}},
                        editError, error),
              "the relink of the camera file applies");
        const int32_t after = ColorGradeNeutralKelvin(ColorGradeContextFor(
            relinked.MakeDocument(relinked.timelines[0].id), clip));
        Check(before == kNeutralKelvin && after == before &&
                  relinked.rushes[0].white_balance_kelvin == 0 &&
                  relinked.rushes[0].path == "../Prises/C0102.MP4",
              "after the relink a color.temperature of the media adapts to "
              "the same neutral as before it");

        // A balance set by hand and an input declared for the media are
        // the project's too: a camera that says otherwise does not undo
        // them.
        LibraryMedia decided = original;
        decided.white_balance_kelvin = 5600;
        decided.color_input = {"rec709", "rec709", "bt709", "limited"};
        LibraryMedia camera = probed;
        camera.white_balance_kelvin = 4000;
        camera.color_input = {"sony_sgamut3_cine", "sony_slog3", "auto",
                              "auto"};
        KeepMediaReading(decided, camera);
        Check(camera.white_balance_kelvin == 5600 &&
                  camera.color_input.transfer == "rec709" &&
                  camera.color_input.range == "limited",
              "a relink keeps a balance and an input set for the media");
    }

    if (failures) return 1;
    std::cout << "PASS: relink compatibility and transactional identity\n";
    return 0;
}
