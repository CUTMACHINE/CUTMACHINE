// PA1 -- docs/roadmap/PARITE-2026-11.md. Une timeline écrite en OTIO et en
// FCPXML, comparée à ce que chaque format demande, écrit ici à la main.
// Rien ne touche un autre éditeur : ce qui est vérifié est le format, pas
// un import.

#include "Document.h"
#include "Interchange.h"
#include "Json.h"

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using mcp_json::Value;

int failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

void CheckText(const std::string& actual, const std::string& expected,
               const std::string& message) {
    if (actual == expected) return;
    ++failures;
    std::cerr << "FAIL: " << message << "\n  expected: " << expected
              << "\n  actual:   " << actual << '\n';
}

constexpr MediaRate k25{25, 1};
constexpr MediaRate k50{50, 1};

const char* kMediaA = "01K70000000000000000000001";
const char* kMediaB = "01K70000000000000000000002";
const char* kVideo1 = "01K70000000000000000000010";
const char* kVideo2 = "01K70000000000000000000011";
const char* kAudio1 = "01K70000000000000000000012";
const char* kClipA = "01K70000000000000000000020";
const char* kClipB = "01K70000000000000000000021";
const char* kClipOff = "01K70000000000000000000022";
const char* kClipSound = "01K70000000000000000000030";
const char* kDissolve = "01K70000000000000000000040";
const char* kMarkerRed = "01K70000000000000000000050";
const char* kMarkerAmber = "01K70000000000000000000051";

LibraryMedia Media(const std::string& id, const std::string& name,
                   MediaRate rate, int64_t frames, int32_t width,
                   int32_t height, bool audio) {
    LibraryMedia media;
    media.id = id;
    media.filename = name;
    media.path = "/rushes/" + name;
    media.codec = "h264";
    media.orientation = "landscape";
    media.width = width;
    media.height = height;
    media.rate = rate;
    media.duration = {frames * rate.den, rate.num};
    media.has_audio = audio;
    media.audio_rate = audio ? 48000 : 0;
    media.audio_channels = audio ? 2 : 0;
    return media;
}

// A clip on whole frames: the record side in the sequence's, the source
// in-point in `sourceRate`'s.
DocumentClip Clip(const std::string& id, const std::string& sourceId,
                  int64_t recordIn, int64_t duration, MediaRate sourceRate,
                  int64_t sourceIn, EffectParamValue speed = {1, 1}) {
    DocumentClip clip;
    clip.id = id;
    clip.source_id = sourceId;
    clip.timeline_in = {recordIn, 25};
    clip.duration = {duration, 25};
    clip.source_in = {sourceIn * sourceRate.den, sourceRate.num};
    clip.speed = speed;
    return clip;
}

DocumentTrack Track(const std::string& id, const std::string& kind,
                    int32_t index, std::vector<DocumentClip> clips) {
    DocumentTrack track;
    track.id = id;
    track.kind = kind;
    track.index = index;
    track.clips = std::move(clips);
    return track;
}

// Two video tracks, one audio track, a slow motion read from a 50 i/s
// rush, a disabled clip after a gap, a dissolve and two markers.
Document Film() {
    Document document;
    document.sequence.id = "01K70000000000000000000009";
    document.sequence.name = "FILM";
    document.sequence.frame_rate = k25;
    document.library = {Media(kMediaA, "A.MP4", k25, 500, 1920, 1080, true),
                        Media(kMediaB, "B.MOV", k50, 1000, 3840, 2160, false)};
    DocumentClip disabled = Clip(kClipOff, kMediaA, 25, 25, k25, 0);
    disabled.enabled = false;
    document.sequence.tracks = {
        Track(kVideo1, "video", 0,
              {Clip(kClipA, kMediaA, 0, 100, k25, 10),
               Clip(kClipB, kMediaB, 100, 50, k50, 50, {1, 2})}),
        Track(kVideo2, "video", 1, {disabled}),
        Track(kAudio1, "audio", 2,
              {Clip(kClipSound, kMediaA, 0, 100, k25, 10)})};
    DocumentTransition dissolve;
    dissolve.id = kDissolve;
    dissolve.track_id = kVideo1;
    dissolve.left_clip_id = kClipA;
    dissolve.right_clip_id = kClipB;
    dissolve.type = "cross_dissolve";
    dissolve.duration = {10, 25};
    document.sequence.transitions = {dissolve};
    DocumentMarker red;
    red.id = kMarkerRed;
    red.name = "Note";
    red.time = {50, 25};
    red.color = "#ff0000";
    DocumentMarker amber;
    amber.id = kMarkerAmber;
    amber.name = "Retake <2>";
    amber.time = {75, 25};
    amber.duration = {25, 25};
    document.sequence.markers = {red, amber};
    return document;
}

const Value& At(const Value& value, const std::string& key) {
    static const Value missing;
    const Value* found = value.Find(key);
    return found == nullptr ? missing : *found;
}

const Value& Item(const Value& array, size_t index) {
    static const Value missing;
    return array.IsArray() && index < array.AsArray().size()
               ? array.AsArray()[index]
               : missing;
}

// "<value>@<rate>" of an OTIO RationalTime.
std::string TimeText(const Value& time) {
    return At(time, "value").Dump() + "@" + At(time, "rate").Dump();
}

void TestSpellings() {
    CheckText(FormatInterchangeDecimal(25, 1), "25", "a whole cadence");
    CheckText(FormatInterchangeDecimal(30000, 1001), "29.970029970029",
              "29.97 truncated at twelve places, never rounded");
    CheckText(FormatInterchangeDecimal(1, 2), "0.5", "a half speed");
    CheckText(FormatInterchangeDecimal(-1, 3), "-0.333333333333",
              "a negative fraction");
    CheckText(FormatInterchangeDecimal(0, 5), "0", "zero");
    CheckText(FormatInterchangeDecimal(-1, 100000000000000LL), "0",
              "a negative value that truncates to zero is not -0");
    CheckText(FormatFcpxmlTime({0, 1}), "0s", "zero seconds");
    CheckText(FormatFcpxmlTime({125, 25}), "5s", "whole seconds");
    CheckText(FormatFcpxmlTime({1001, 30000}), "1001/30000s",
              "one frame at 29.97");
    CheckText(FormatFcpxmlTime({10, 48000}), "1/4800s", "a reduced fraction");
}

void TestBuild() {
    InterchangeTimeline timeline;
    std::string error;
    Check(BuildInterchangeTimeline(Film(), timeline, error),
          "the film builds: " + error);
    Check(timeline.tracks.size() == 3, "two video tracks and one audio track");
    if (timeline.tracks.size() != 3) return;
    Check(timeline.tracks[0].video && timeline.tracks[0].number == 1 &&
              timeline.tracks[1].video && timeline.tracks[1].number == 2 &&
              !timeline.tracks[2].video && timeline.tracks[2].number == 1,
          "tracks are numbered from the bottom of their kind");
    Check(timeline.media.size() == 2, "each rush once, whoever reads it");
    Check(timeline.duration == RationalTime(150, 25),
          "the timeline ends with its last clip");
    Check(!timeline.tracks[1].clips.empty() &&
              !timeline.tracks[1].clips[0].enabled,
          "a disabled clip travels disabled");
    Check(
        timeline.tracks[0].transitions.size() == 1 &&
            timeline.tracks[0].transitions[0].before_cut ==
                RationalTime(5, 25) &&
            timeline.tracks[0].transitions[0].after_cut == RationalTime(5, 25),
        "a centred dissolve of ten frames covers five on each side");
    Check(timeline.losses.empty(), "a film of plain cuts loses nothing");

    Document graded = Film();
    DocumentClip& first = graded.sequence.tracks[0].clips[0];
    first.effects.push_back({});
    first.opacity = {1, 2};
    first.blur = {4, 1};
    // PA8 -- docs/roadmap/PARITE-2026-11.md.
    first.mask.shape = kClipMaskEllipse;
    graded.sequence.tracks[2].clips[0].audio_gain_db = {-3, 1};
    graded.sequence.tracks[2].clips[0].audio_fade_in = {5, 25};
    graded.sequence.tracks[1].visible = false;
    graded.sequence.look_lut.digest = "abc";
    graded.sequence.tracks.push_back(
        Track("01K70000000000000000000013", "caption", 3, {}));
    Check(BuildInterchangeTimeline(graded, timeline, error),
          "the graded film builds: " + error);
    std::string kinds;
    for (const InterchangeLoss& loss : timeline.losses)
        kinds += loss.kind + " ";
    CheckText(kinds,
              "look_lut caption_track grade opacity blur mask hidden_track "
              "audio_gain audio_fade ",
              "what stays behind is named, in the order it is met");
    Check(!timeline.tracks[1].enabled && !timeline.tracks[1].clips[0].enabled,
          "a hidden track and its clips travel disabled");

    Document unknown = Film();
    unknown.library.pop_back();
    Check(!BuildInterchangeTimeline(unknown, timeline, error) &&
              error.find(kMediaB) != std::string::npos,
          "a clip whose media the library does not hold is refused by name");

    Document empty = Film();
    for (DocumentTrack& track : empty.sequence.tracks) track.clips.clear();
    empty.sequence.transitions.clear();
    Check(!BuildInterchangeTimeline(empty, timeline, error),
          "a timeline with no clip is refused");

    Document solo = Film();
    solo.sequence.tracks.push_back(
        Track("01K70000000000000000000014", "audio", 4,
              {Clip("01K70000000000000000000031", kMediaA, 0, 50, k25, 0)}));
    solo.sequence.tracks.back().solo = true;
    Check(BuildInterchangeTimeline(solo, timeline, error) &&
              timeline.tracks.size() == 4 && !timeline.tracks[2].enabled &&
              timeline.tracks[3].enabled,
          "a soloed audio track silences the others, as the mix does");
}

void TestOtio() {
    InterchangeTimeline timeline;
    std::string error;
    if (!BuildInterchangeTimeline(Film(), timeline, error)) {
        Check(false, "the film builds: " + error);
        return;
    }
    const OtioExport exported = WriteOtio(timeline);
    Value root;
    Check(Value::Parse(exported.text, root, error),
          "the .otio is JSON: " + error);
    CheckText(At(root, "OTIO_SCHEMA").AsString(), "Timeline.1",
              "the root is a timeline");
    CheckText(At(root, "name").AsString(), "FILM", "named as the sequence");
    const Value& stack = At(root, "tracks");
    CheckText(At(stack, "OTIO_SCHEMA").AsString(), "Stack.1",
              "tracks are a stack");
    const Value& tracks = At(stack, "children");
    Check(tracks.IsArray() && tracks.AsArray().size() == 3,
          "three tracks in the stack");

    const Value& video1 = Item(tracks, 0);
    CheckText(At(video1, "kind").AsString(), "Video", "V1 is a video track");
    CheckText(At(video1, "name").AsString(), "V1", "V1 is named V1");
    const Value& children = At(video1, "children");
    Check(children.IsArray() && children.AsArray().size() == 3,
          "V1 holds a clip, a transition and a clip");
    const Value& first = Item(children, 0);
    CheckText(At(first, "OTIO_SCHEMA").AsString(), "Clip.2", "a clip first");
    CheckText(TimeText(At(At(first, "source_range"), "start_time")), "10@25",
              "the source in-point in the media's frames");
    CheckText(TimeText(At(At(first, "source_range"), "duration")), "100@25",
              "the length on the timeline");
    CheckText(
        At(At(At(first, "media_references"), "DEFAULT_MEDIA"), "target_url")
            .AsString(),
        "file:///rushes/A.MP4", "the rush as a file URL");
    CheckText(At(At(At(first, "metadata"), "cutmachine"), "clip_id").AsString(),
              kClipA, "the clip keeps its identity in the metadata");

    const Value& transition = Item(children, 1);
    CheckText(At(transition, "OTIO_SCHEMA").AsString(), "Transition.1",
              "the dissolve sits between its two clips");
    CheckText(At(transition, "transition_type").AsString(), "SMPTE_Dissolve",
              "as OTIO's dissolve");
    CheckText(TimeText(At(transition, "in_offset")), "5@25",
              "five frames of the outgoing clip");
    CheckText(TimeText(At(transition, "out_offset")), "5@25",
              "five frames of the incoming clip");

    const Value& slow = Item(children, 2);
    CheckText(TimeText(At(At(slow, "source_range"), "start_time")), "50@50",
              "a 50 i/s rush counts its in-point in its own frames");
    CheckText(TimeText(At(At(slow, "source_range"), "duration")), "50@25",
              "and its length in the sequence's");
    const Value& warp = Item(At(slow, "effects"), 0);
    CheckText(At(warp, "OTIO_SCHEMA").AsString(), "LinearTimeWarp.1",
              "a constant speed is a linear time warp");
    CheckText(At(warp, "time_scalar").Dump(), "0.5", "at the clip's speed");

    const Value& above = At(Item(tracks, 1), "children");
    Check(above.IsArray() && above.AsArray().size() == 2,
          "V2 holds a gap and a clip");
    CheckText(At(Item(above, 0), "OTIO_SCHEMA").AsString(), "Gap.1",
              "the empty second is a gap");
    CheckText(TimeText(At(At(Item(above, 0), "source_range"), "duration")),
              "25@25", "one second long");
    Check(At(Item(above, 1), "enabled").IsBool() &&
              !At(Item(above, 1), "enabled").AsBool(),
          "the disabled clip is written disabled");

    CheckText(At(Item(tracks, 2), "kind").AsString(), "Audio",
              "the sound is an audio track");
    CheckText(At(Item(tracks, 2), "name").AsString(), "A1", "named A1");

    const Value& markers = At(stack, "markers");
    Check(markers.IsArray() && markers.AsArray().size() == 2, "two markers");
    CheckText(At(Item(markers, 0), "color").AsString(), "RED",
              "a pure red is OTIO's RED");
    CheckText(At(Item(markers, 1), "color").AsString(), "ORANGE",
              "the document's default amber is nearest ORANGE");
    CheckText(TimeText(At(At(Item(markers, 1), "marked_range"), "duration")),
              "25@25", "a span keeps its length");
    Check(exported.colors_approximated ==
              std::vector<Ulid>{std::string(kMarkerAmber)},
          "the approximated colour is named");
    Check(exported.off_frame_times == 0, "every time sits on a frame");
    Check(exported.transitions_not_sent.empty(), "the dissolve was sent");

    // A sound cut inside a frame, and a fade at the edge of a track.
    Document loose = Film();
    loose.sequence.tracks[2].clips[0].source_in = {1001, 48000};
    DocumentTransition fade;
    fade.id = "01K70000000000000000000041";
    fade.track_id = kVideo1;
    fade.right_clip_id = kClipA;
    fade.type = "dip_to_color";
    fade.duration = {10, 25};
    loose.sequence.transitions.push_back(fade);
    if (!BuildInterchangeTimeline(loose, timeline, error)) {
        Check(false, "the loose film builds: " + error);
        return;
    }
    const OtioExport second = WriteOtio(timeline);
    Check(Value::Parse(second.text, root, error),
          "the second .otio is JSON: " + error);
    const Value& sound =
        Item(At(Item(At(At(root, "tracks"), "children"), 2), "children"), 0);
    CheckText(TimeText(At(At(sound, "source_range"), "start_time")),
              "1001@48000", "a time inside a frame is written in its own rate");
    Check(second.off_frame_times == 1, "and counted");
    Check(second.transitions_not_sent ==
              std::vector<Ulid>{std::string("01K70000000000000000000041")},
          "an edge fade has no place in an OTIO track and is named");
}

void TestFcpxml() {
    InterchangeTimeline timeline;
    std::string error;
    if (!BuildInterchangeTimeline(Film(), timeline, error)) {
        Check(false, "the film builds: " + error);
        return;
    }
    const FcpxmlExport exported = WriteFcpxml(timeline);
    const std::string expected =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE fcpxml>\n"
        "<fcpxml version=\"1.10\">\n"
        "    <resources>\n"
        "        <format id=\"r1\" frameDuration=\"1/25s\" width=\"1920\" "
        "height=\"1080\"/>\n"
        "        <format id=\"r2\" frameDuration=\"1/50s\" width=\"3840\" "
        "height=\"2160\"/>\n"
        "        <asset id=\"r3\" name=\"A.MP4\" start=\"0s\" "
        "duration=\"20s\" hasVideo=\"1\" hasAudio=\"1\" format=\"r1\" "
        "audioSources=\"1\" audioChannels=\"2\" audioRate=\"48000\">\n"
        "            <media-rep kind=\"original-media\" "
        "src=\"file:///rushes/A.MP4\"/>\n"
        "        </asset>\n"
        "        <asset id=\"r4\" name=\"B.MOV\" start=\"0s\" "
        "duration=\"20s\" hasVideo=\"1\" hasAudio=\"0\" format=\"r2\">\n"
        "            <media-rep kind=\"original-media\" "
        "src=\"file:///rushes/B.MOV\"/>\n"
        "        </asset>\n"
        "    </resources>\n"
        "    <library>\n"
        "        <event name=\"CUTMACHINE\">\n"
        "            <project name=\"FILM\">\n"
        "                <sequence format=\"r1\" duration=\"6s\" "
        "tcStart=\"0s\" tcFormat=\"NDF\" audioLayout=\"stereo\" "
        "audioRate=\"48k\">\n"
        "                    <spine>\n"
        "                        <gap name=\"Gap\" offset=\"0s\" start=\"0s\" "
        "duration=\"6s\">\n"
        "                            <asset-clip ref=\"r3\" lane=\"1\" "
        "offset=\"0s\" name=\"A.MP4\" start=\"2/5s\" duration=\"4s\" "
        "format=\"r1\" srcEnable=\"video\"/>\n"
        // Half speed: the in-point one second into the rush is two seconds
        // into the clip's own, retimed time, and two seconds of timeline
        // read one second of source.
        "                            <asset-clip ref=\"r4\" lane=\"1\" "
        "offset=\"4s\" name=\"B.MOV\" start=\"2s\" duration=\"2s\" "
        "format=\"r2\" srcEnable=\"video\">\n"
        "                                <timeMap>\n"
        "                                    <timept time=\"2s\" "
        "value=\"1s\" interp=\"linear\"/>\n"
        "                                    <timept time=\"4s\" "
        "value=\"2s\" interp=\"linear\"/>\n"
        "                                </timeMap>\n"
        "                            </asset-clip>\n"
        "                            <asset-clip ref=\"r3\" lane=\"2\" "
        "offset=\"1s\" name=\"A.MP4\" start=\"0s\" duration=\"1s\" "
        "format=\"r1\" srcEnable=\"video\" enabled=\"0\"/>\n"
        "                            <asset-clip ref=\"r3\" lane=\"-1\" "
        "offset=\"0s\" name=\"A.MP4\" start=\"2/5s\" duration=\"4s\" "
        "format=\"r1\" srcEnable=\"audio\"/>\n"
        "                            <marker start=\"2s\" duration=\"1/25s\" "
        "value=\"Note\"/>\n"
        "                            <marker start=\"3s\" duration=\"1s\" "
        "value=\"Retake &lt;2&gt;\"/>\n"
        "                        </gap>\n"
        "                    </spine>\n"
        "                </sequence>\n"
        "            </project>\n"
        "        </event>\n"
        "    </library>\n"
        "</fcpxml>\n";
    CheckText(exported.text, expected, "the film as FCPXML 1.10");
    Check(exported.transitions_not_sent ==
              std::vector<Ulid>{std::string(kDissolve)},
          "no transition travels in this layout, and each is named");

    // A held frame maps both ends of the clip to one source time.
    Document held = Film();
    held.sequence.tracks[0].clips[1].speed = {0, 1};
    if (!BuildInterchangeTimeline(held, timeline, error)) {
        Check(false, "the held film builds: " + error);
        return;
    }
    const std::string text = WriteFcpxml(timeline).text;
    Check(
        text.find("start=\"0s\" duration=\"2s\" format=\"r2\"") !=
                std::string::npos &&
            text.find("<timept time=\"0s\" value=\"1s\"") !=
                std::string::npos &&
            text.find("<timept time=\"2s\" value=\"1s\"") != std::string::npos,
        "a held frame holds one source time over its whole length");
    const OtioExport otio = WriteOtio(timeline);
    Check(otio.text.find("\"FreezeFrame.1\"") != std::string::npos &&
              otio.text.find("\"time_scalar\":0") != std::string::npos,
          "and is a FreezeFrame effect in OTIO");
}

void TestFiles() {
    const std::filesystem::path directory =
        std::filesystem::canonical(std::filesystem::temp_directory_path()) /
        ("cutmachine-interchange-" + std::to_string(::getpid()));
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    Document document = Film();
    document.library[0].path = "Media/A.MP4";

    const std::string otio = (directory / "film.otio").string();
    std::string report;
    std::string errorName;
    std::string error;
    Check(ExportOtioFile(document, directory.string(), otio, report, errorName,
                         error),
          "the .otio is written: " + error);
    Value parsed;
    Check(Value::Parse(report, parsed, error), "its report is JSON: " + error);
    CheckText(At(parsed, "schema").AsString(), "cutmachine.otio.v1",
              "the report names its schema");
    CheckText(At(parsed, "clips").Dump(), "4", "four clips counted");
    CheckText(At(parsed, "retimed_clips").Dump(), "1", "one retimed");
    CheckText(At(parsed, "disabled_clips").Dump(), "1", "one disabled");
    CheckText(At(parsed, "transitions_not_sent").Dump(), "0",
              "the dissolve sent");
    Check(At(parsed, "verified_in_editor").IsBool() &&
              !At(parsed, "verified_in_editor").AsBool(),
          "the report says no editor has read this");
    std::ifstream input(otio, std::ios::binary);
    std::ostringstream contents;
    contents << input.rdbuf();
    Check(contents.str().find("\"target_url\":\"file://" +
                              (directory / "Media" / "A.MP4").string() +
                              "\"") != std::string::npos,
          "a path stored relative to the package is written absolute");

    Check(!ExportOtioFile(document, directory.string(), otio, report, errorName,
                          error) &&
              errorName == "OutputExists",
          "an existing destination is refused, not replaced");
    Check(!ExportOtioFile(document, directory.string(),
                          (directory / "film.json").string(), report, errorName,
                          error) &&
              errorName == "ValidationFailed",
          "a path that is not a .otio is refused");
    Check(!ExportOtioFile(document, directory.string(), "film.otio", report,
                          errorName, error) &&
              errorName == "ValidationFailed",
          "a relative path is refused");

    const std::string fcpxml = (directory / "film.fcpxml").string();
    Check(ExportFcpxmlFile(document, directory.string(), fcpxml, report,
                           errorName, error),
          "the .fcpxml is written: " + error);
    Check(Value::Parse(report, parsed, error), "its report is JSON: " + error);
    CheckText(At(parsed, "schema").AsString(), "cutmachine.fcpxml.v1",
              "the report names its schema");
    CheckText(At(parsed, "transitions_not_sent").Dump(), "1",
              "the dissolve is counted as left behind");
    bool named = false;
    for (const Value& loss : At(parsed, "losses").AsArray())
        named = named || (At(loss, "kind").AsString() == "transition" &&
                          At(loss, "id").AsString() == kDissolve);
    Check(named, "and named among the losses");
    Check(!ExportFcpxmlFile(document, directory.string(), otio, report,
                            errorName, error) &&
              errorName == "ValidationFailed",
          "a path that is not a .fcpxml is refused");

    Document unknown = Film();
    unknown.library.clear();
    Check(!ExportFcpxmlFile(unknown, directory.string(),
                            (directory / "other.fcpxml").string(), report,
                            errorName, error) &&
              errorName == "InvalidExport" &&
              !std::filesystem::exists(directory / "other.fcpxml"),
          "a refused montage writes no file");
    std::filesystem::remove_all(directory);
}

}  // namespace

int main() {
    TestSpellings();
    TestBuild();
    TestOtio();
    TestFcpxml();
    TestFiles();
    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "interchange: ok\n";
    return 0;
}
