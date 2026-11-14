#include "AudioPlayback.h"
#include "Json.h"
#include "PlaybackPresentation.h"
#include "TimelineView.h"
#include "Ulid.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

std::string Quote(const std::filesystem::path& path) {
    std::string result = "'";
    for (char character : path.string())
        result += character == '\'' ? "'\\''" : std::string(1, character);
    return result + "'";
}

}  // namespace

int main() {
    const std::filesystem::path root = std::filesystem::temp_directory_path() /
                                       (GenerateUlid() + "-audio-playback");
    const std::filesystem::path media = root / "tone.mp4";
    std::filesystem::create_directories(root);
    const std::string generate =
        Quote(FFMPEG_EXECUTABLE) +
        " -hide_banner -loglevel error "
        "-f lavfi -i 'color=c=black:s=32x32:r=25:d=1' "
        "-f lavfi -i 'sine=frequency=440:sample_rate=48000:duration=1' "
        "-c:v mpeg4 -c:a aac -shortest " +
        Quote(media);
    if (std::system(generate.c_str()) != 0) {
        std::cerr << "FAIL: unable to generate audio fixture\n";
        return 1;
    }

    Document document;
    const Ulid sourceId = "01KA0000000000000000000001";
    const Ulid unusedSourceId = "01KA0000000000000000000006";
    document.sources = {
        {sourceId, "tone.mp4", {25, 1}, {25, 25}},
        {unusedSourceId, "tone.mp4", {25, 1}, {25, 25}},
    };
    document.sequence.tracks = {
        {"01KA0000000000000000000002",
         "video",
         0,
         {{"01KA0000000000000000000003",
           sourceId,
           {0, 25},
           {25, 25},
           {0, 25},
           true}}},
    };
    AudioPlayback playback;
    std::string error;
    const bool opened = playback.Open(document, root.string(), error);
    const bool unusedBeforeInsert =
        playback.DecodedSourceCount() == 0 && playback.PlannedClipCount() == 0;
    document.sequence.tracks.push_back({"01KA0000000000000000000004",
                                        "audio",
                                        1,
                                        {{"01KA0000000000000000000005",
                                          sourceId,
                                          {0, 25},
                                          {25, 25},
                                          {0, 25},
                                          true}}});
    playback.RebuildTimeline(document);
    // PERF-2026-09. RebuildTimeline no longer decodes on the calling thread,
    // so the mix a rebuild publishes holds only the sources decoded so far.
    // Everything below asserts the settled mix, and waits for it explicitly.
    const bool decodesSettled = playback.WaitForDecodes(10000);
    const bool decoded = playback.DecodedSourceCount() == 1;
    // One second of stereo at 48 kHz, held as 16-bit samples and trimmed to
    // what was decoded: 192 000 bytes and the codec's padding, where float
    // samples grown packet by packet held two to four times that.
    const size_t decodedBytes = playback.DecodedBytes();
    const bool compact =
        decodedBytes >= 4u * 44000u && decodedBytes <= 4u * (48000u + 4096u);
    const bool planned = playback.PlannedClipCount() == 1;
    document.sequence.tracks[1].muted = true;
    playback.RebuildTimeline(document);
    const bool muteSuppressesTrack = playback.PlannedClipCount() == 0;
    document.sequence.tracks[1].muted = false;
    DocumentTrack secondAudio = document.sequence.tracks[1];
    secondAudio.id = "01KA0000000000000000000007";
    secondAudio.index = 2;
    secondAudio.clips[0].id = "01KA0000000000000000000008";
    document.sequence.tracks.push_back(secondAudio);
    document.sequence.tracks[1].solo = true;
    playback.RebuildTimeline(document);
    const bool soloSuppressesOtherTracks = playback.PlannedClipCount() == 1;
    document.sequence.tracks[1].solo = false;
    playback.RebuildTimeline(document);
    const bool allUnmutedTracksMix = playback.PlannedClipCount() == 2;

    // PERF-2026-09. Decoded audio outlives the timeline that made it
    // audible. It used to be dropped the moment a rebuild stopped naming it,
    // which is what made switching away from a timeline and back decode
    // every rush again -- minutes of work on an external drive, for material
    // that was already in memory. The second rebuild below therefore asserts
    // a *full* mix with no wait at all: had the source been dropped, the
    // plan would be empty until a re-decode landed.
    const std::vector<DocumentTrack> audioTracks(
        document.sequence.tracks.begin() + 1, document.sequence.tracks.end());
    document.sequence.tracks.resize(1);
    playback.RebuildTimeline(document);
    const bool retainedWhenSilent =
        playback.DecodedSourceCount() == 1 && playback.PlannedClipCount() == 0;
    for (const DocumentTrack& track : audioTracks)
        document.sequence.tracks.push_back(track);
    playback.RebuildTimeline(document);
    const bool retainedMixesWithoutDecoding = playback.PlannedClipCount() == 2;
    const bool started = opened && playback.PlayFrom({0, 25}, 2, error);
    const bool shuttleSpeedPreserved = playback.ShuttleSpeed() == 2;
    playback.Stop();
    // PRD37 -- ROADMAP.md. A stop silences the output and leaves it open:
    // closing it there made the next play wait 100 to 200 ms for the device,
    // the playback clock already running. It closes once silent for the
    // whole grace, and the next play opens it again.
    const uint64_t startsBefore = playback.OutputStartCount();
    const bool openAfterStop = playback.OutputOpen();
    const bool playedAgain = playback.PlayFrom({5, 25}, 1, error);
    const bool notReopened = playback.OutputStartCount() == startsBefore;
    playback.Stop();
    playback.ReleaseIdleOutput(std::chrono::steady_clock::now());
    const bool keptWithinGrace = playback.OutputOpen();
    playback.ReleaseIdleOutput(std::chrono::steady_clock::now() +
                               playback::kAudioOutputIdleGrace);
    const bool closedAfterGrace = !playback.OutputOpen();
    const bool reopened = playback.PlayFrom({0, 25}, 1, error) &&
                          playback.OutputOpen() &&
                          playback.OutputStartCount() == startsBefore + 1;
    playback.Stop();
    const bool outputKeptBetweenPlays =
        started && openAfterStop && playedAgain && notReopened &&
        keptWithinGrace && closedAfterGrace && reopened;
    const RationalTime firstMousePosition = QuantizePlayheadPosition(
        {121, 250}, PlayheadResolution::Frame, {25, 1});
    const RationalTime sameFrameMousePosition = QuantizePlayheadPosition(
        {124, 250}, PlayheadResolution::Frame, {25, 1});
    const RationalTime nextFrameMousePosition = QuantizePlayheadPosition(
        {126, 250}, PlayheadResolution::Frame, {25, 1});
    const bool scrubbed = opened && playback.ScrubAt(firstMousePosition, error);
    const bool sameFrameSuppressed =
        firstMousePosition == sameFrameMousePosition &&
        playback.ScrubAt(sameFrameMousePosition, error) &&
        playback.ScrubTriggerCount() == 1;
    const bool nextFrameTriggered =
        nextFrameMousePosition != firstMousePosition &&
        playback.ScrubAt(nextFrameMousePosition, error) &&
        playback.ScrubTriggerCount() == 2;
    playback.Stop();

    // REMOTION-PARITY-2026-09 REM2 -- a motion clip whose composition plays
    // a sound joins the mix: the engine renders its part of the
    // composition's mix on the decode thread, and a hidden video track
    // plays nothing, as it exports nothing.
    Document motion;
    motion.sources = {{sourceId, "tone.mp4", {25, 1}, {25, 25}}};
    mcp_json::Value definition;
    std::string parseError;
    mcp_json::Value::Parse(
        R"({"width":32,"height":32,"fps":25,"duration":25,)"
        R"("root":{"type":"fill","uid":"01KA0000000000000000000011",)"
        R"("children":[{"type":"audio","uid":"01KA0000000000000000000012",)"
        R"("media":"01KA0000000000000000000001"}]}})",
        definition, parseError);
    motion.compositions = {{"01KA0000000000000000000010", "Son", definition}};
    DocumentClip motionClip;
    motionClip.id = "01KA0000000000000000000013";
    motionClip.composition_id = "01KA0000000000000000000010";
    motionClip.source_in = {5, 25};
    motionClip.duration = {15, 25};
    motionClip.timeline_in = {0, 25};
    motionClip.include_audio = false;
    motion.sequence.tracks = {
        {"01KA0000000000000000000014", "video", 0, {motionClip}}};
    std::string motionError;
    const bool motionValid = motion.Validate(motionError);
    AudioPlayback composed;
    const bool composedOpened = composed.Open(motion, root.string(), error);
    composed.RebuildTimeline(motion);
    const bool motionSettled = composed.WaitForDecodes(10000);
    const bool motionMixed =
        composed.PlannedClipCount() == 1 && composed.DecodedSourceCount() == 1;
    motion.sequence.tracks[0].visible = false;
    composed.RebuildTimeline(motion);
    const bool hiddenSilent = composed.PlannedClipCount() == 0;
    composed.Stop();
    std::filesystem::remove_all(root);
    if (!motionValid || !composedOpened || !motionSettled || !motionMixed ||
        !hiddenSilent) {
        std::cerr << "FAIL: a motion clip's sound joins the mix: "
                  << motionError << ' ' << error << ' ' << parseError << '\n';
        return 1;
    }
    if (!compact) {
        std::cerr << "FAIL: one second of decoded stereo holds " << decodedBytes
                  << " bytes, not about 192 000\n";
        return 1;
    }
    if (!outputKeptBetweenPlays) {
        std::cerr << "FAIL: a stop leaves the output open for the next play, "
                     "and it closes after the grace (open after stop "
                  << openAfterStop << ", reopened by the play " << !notReopened
                  << ", kept within the grace " << keptWithinGrace
                  << ", closed after it " << closedAfterGrace
                  << ", opened again " << reopened << "): " << error << '\n';
        return 1;
    }
    std::cout << "PASS: a stop leaves the output open for the next play, and "
                 "it closes after the grace\n";
    if (!opened || !unusedBeforeInsert || !decodesSettled || !decoded ||
        !planned || !muteSuppressesTrack || !soloSuppressesOtherTracks ||
        !allUnmutedTracksMix || !retainedWhenSilent ||
        !retainedMixesWithoutDecoding || !started || !shuttleSpeedPreserved ||
        !scrubbed || !sameFrameSuppressed || !nextFrameTriggered) {
        std::cerr << "FAIL: audio decode/mix plan: " << error << '\n';
        return 1;
    }
    std::cout << "PASS: audio source decodes and enters the timeline mix\n";
    return 0;
}
