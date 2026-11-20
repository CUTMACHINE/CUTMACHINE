// FC7 -- docs/roadmap/FILMCRAFT-2026-10.md. Les listes CMX 3600 d'une
// timeline et de ses marqueurs, comparées à des EDL écrites à la main
// (tests/fixtures/edl), et les marqueurs relus en opérations. Rien ici ne
// touche Resolve : ce qui est vérifié est le format, pas un import.

#include "ClipSpeed.h"
#include "Document.h"
#include "EditLog.h"
#include "Edl.h"
#include "Operations.h"
#include "TimelineView.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "TestSupport.h"

namespace {

std::string Fixture(const std::string& name) {
    std::ifstream input(std::string(EDL_FIXTURES_DIR) + "/" + name,
                        std::ios::binary);
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

LibraryMedia Media(const std::string& id, const std::string& name,
                   MediaRate rate, int64_t frames) {
    LibraryMedia media;
    media.id = id;
    media.filename = name;
    media.path = "/rushes/" + name;
    media.codec = "h264";
    media.orientation = "landscape";
    media.width = 1920;
    media.height = 1080;
    media.rate = rate;
    media.duration = {frames * rate.den, rate.num};
    return media;
}

// A clip placed in whole frames of `rate`.
DocumentClip Clip(const std::string& id, const std::string& sourceId,
                  MediaRate rate, int64_t recordIn, int64_t sourceIn,
                  int64_t duration, EffectParamValue speed = {1, 1}) {
    DocumentClip clip;
    clip.id = id;
    clip.source_id = sourceId;
    clip.timeline_in = {recordIn * rate.den, rate.num};
    clip.source_in = {sourceIn * rate.den, rate.num};
    clip.duration = {duration * rate.den, rate.num};
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

constexpr MediaRate k25{25, 1};
constexpr MediaRate k23976{24000, 1001};
constexpr MediaRate k2997{30000, 1001};

const char* kMediaA = "01K70000000000000000000001";
const char* kMediaB = "01K70000000000000000000002";

Document Cuts25() {
    Document document;
    document.sequence.name = "FILM";
    document.sequence.frame_rate = k25;
    document.library = {Media(kMediaA, "A.MP4", k25, 500),
                        Media(kMediaB, "B.MOV", k25, 500)};
    document.sequence.tracks = {
        Track("01K70000000000000000000010", "video", 0,
              {Clip("01K70000000000000000000020", kMediaA, k25, 0, 10, 100),
               Clip("01K70000000000000000000021", kMediaB, k25, 100, 0, 50)})};
    // The sources an edit validates against, beside the library.
    for (const LibraryMedia& media : document.library)
        document.sources.push_back(
            {media.id, media.path, media.rate, media.duration});
    return document;
}

bool Build(const Document& document, EdlExport& exported, std::string& error,
           const std::string& start = kEdlDefaultStartTimecode) {
    EdlExportOptions options;
    options.start_timecode = start;
    return BuildEdlExport(document, options, exported, error);
}

void ExpectList(const Document& document, const std::vector<std::string>& names,
                const std::string& what) {
    EdlExport exported;
    std::string error;
    const bool built = Build(document, exported, error);
    Check(built, what + " builds: " + error);
    if (!built) return;
    Check(exported.files.size() == names.size(),
          what + " writes one list per video track");
    for (size_t index = 0;
         index < names.size() && index < exported.files.size(); ++index) {
        const std::string expected = Fixture(names[index]);
        Check(!expected.empty(), names[index] + " is readable");
        Check(exported.files[index].text == expected,
              what + " matches " + names[index] + ":\n" +
                  exported.files[index].text + "--- expected ---\n" + expected);
    }
}

bool Refused(const Document& document, const std::string& needle,
             const std::string& what) {
    EdlExport exported;
    std::string error;
    const bool built = Build(document, exported, error);
    Check(!built && error.find(needle) != std::string::npos,
          what + " is refused by name ('" + needle + "'): " + error);
    return !built;
}

size_t LossCount(const EdlExport& exported, const std::string& kind,
                 const Ulid& id = {}) {
    size_t count = 0;
    for (const EdlLoss& loss : exported.losses)
        if (loss.kind == kind && (id.empty() || loss.id == id)) ++count;
    return count;
}

void TestTimecodes() {
    EdlTimecodeFormat format;
    std::string error;
    Check(EdlTimecodeFormatFor(k25, format, error) && format.base == 25 &&
              !format.drop_frame,
          "25 i/s counts 25 non-drop");
    Check(EdlTimecodeFormatFor({50, 2}, format, error) && format.base == 25,
          "50/2 is 25");
    Check(EdlTimecodeFormatFor(k23976, format, error) && format.base == 24 &&
              !format.drop_frame,
          "23,976 counts 24 non-drop");
    Check(EdlTimecodeFormatFor(k2997, format, error) && format.base == 30 &&
              format.drop_frame,
          "29,97 counts 30 drop-frame");
    Check(EdlTimecodeFormatFor({60000, 1001}, format, error) &&
              format.base == 60 && format.drop_frame,
          "59,94 counts 60 drop-frame");
    Check(!EdlTimecodeFormatFor({25000, 1001}, format, error) &&
              error.find("25000/1001") != std::string::npos,
          "a cadence without a timecode is refused by name: " + error);

    const EdlTimecodeFormat df30{30, true};
    const EdlTimecodeFormat df60{60, true};
    const struct {
        EdlTimecodeFormat format;
        int64_t frames;
        const char* text;
    } cases[] = {
        {{25, false}, 0, "00:00:00:00"},
        {{25, false}, 90000, "01:00:00:00"},
        {{25, false}, 2159999, "23:59:59:24"},
        {df30, 1799, "00:00:59;29"},
        {df30, 1800, "00:01:00;02"},
        {df30, 17981, "00:09:59;29"},
        {df30, 17982, "00:10:00;00"},
        {df30, 107892, "01:00:00;00"},
        {df60, 3599, "00:00:59;59"},
        {df60, 3600, "00:01:00;04"},
    };
    for (const auto& entry : cases) {
        std::string text;
        Check(FormatEdlTimecode(entry.frames, entry.format, text, error) &&
                  text == entry.text,
              "frame " + std::to_string(entry.frames) + " is " + entry.text +
                  ", not " + text);
        int64_t frames = -1;
        Check(ParseEdlTimecode(entry.text, entry.format, frames, error) &&
                  frames == entry.frames,
              std::string(entry.text) + " reads back as frame " +
                  std::to_string(entry.frames) + ", not " +
                  std::to_string(frames));
    }
    // Every frame of the first eleven minutes, drop-frame, both ways.
    for (int64_t frame = 0; frame < 30 * 60 * 11; ++frame) {
        std::string text;
        int64_t back = -1;
        if (!FormatEdlTimecode(frame, df30, text, error) ||
            !ParseEdlTimecode(text, df30, back, error) || back != frame) {
            Check(false, "drop-frame round trip breaks at frame " +
                             std::to_string(frame) + " (" + text + ")");
            break;
        }
    }
    int64_t frames = 0;
    Check(!ParseEdlTimecode("00:01:00;00", df30, frames, error) &&
              error.find("skips") != std::string::npos,
          "a frame number drop-frame skips is refused: " + error);
    Check(
        ParseEdlTimecode("00:10:00;00", df30, frames, error) && frames == 17982,
        "every tenth minute keeps its first frame numbers");
    Check(!ParseEdlTimecode("00:00:01;00", {25, false}, frames, error),
          "drop-frame on a base of 25 is refused");
    Check(!ParseEdlTimecode("00:00:00:25", {25, false}, frames, error),
          "frame 25 does not exist on a base of 25");
    Check(!ParseEdlTimecode("1:2", {25, false}, frames, error),
          "half a timecode is refused");
    std::string text;
    Check(!FormatEdlTimecode(25 * 86400, {25, false}, text, error) &&
              !FormatEdlTimecode(-1, {25, false}, text, error),
          "no timecode below zero or from 24 hours on");
}

void TestLists() {
    ExpectList(Cuts25(), {"cut_25.edl"}, "two cuts at 25 i/s");

    {
        Document slow;
        slow.sequence.name = "SLOW";
        slow.sequence.frame_rate = k23976;
        slow.library = {Media(kMediaA, "C0001.MP4", k23976, 1000)};
        slow.sequence.tracks = {
            Track("01K70000000000000000000010", "video", 0,
                  {Clip("01K70000000000000000000020", kMediaA, k23976, 0, 24,
                        48, {1, 2})})};
        ExpectList(slow, {"slow_23976.edl"}, "half speed at 23,976");
    }

    {
        Document ntsc;
        ntsc.sequence.name = "NTSC";
        ntsc.sequence.frame_rate = k2997;
        ntsc.library = {Media(kMediaA, "NTSC.MOV", k2997, 1000)};
        ntsc.sequence.tracks = {Track(
            "01K70000000000000000000010", "video", 0,
            {Clip("01K70000000000000000000020", kMediaA, k2997, 1790, 0, 20)})};
        ExpectList(ntsc, {"ntsc_2997df.edl"},
                   "a cut across a dropped minute at 29,97");
    }

    {
        Document fade = Cuts25();
        fade.sequence.name = "FONDU";
        fade.sequence.tracks[0].clips = {
            Clip("01K70000000000000000000020", kMediaA, k25, 0, 0, 100),
            Clip("01K70000000000000000000021", kMediaB, k25, 100, 50, 100)};
        DocumentTransition opening;
        opening.id = "01K70000000000000000000030";
        opening.track_id = "01K70000000000000000000010";
        opening.right_clip_id = "01K70000000000000000000020";
        opening.type = "dip_to_color";
        opening.duration = {10, 25};
        opening.alignment = TransitionAlignment::StartAtCut;
        DocumentTransition dissolve;
        dissolve.id = "01K70000000000000000000031";
        dissolve.track_id = "01K70000000000000000000010";
        dissolve.left_clip_id = "01K70000000000000000000020";
        dissolve.right_clip_id = "01K70000000000000000000021";
        dissolve.type = "cross_dissolve";
        dissolve.duration = {10, 25};
        dissolve.alignment = TransitionAlignment::Center;
        DocumentTransition closing;
        closing.id = "01K70000000000000000000032";
        closing.track_id = "01K70000000000000000000010";
        closing.left_clip_id = "01K70000000000000000000021";
        closing.type = "dip_to_color";
        closing.duration = {25, 25};
        closing.alignment = TransitionAlignment::EndAtCut;
        fade.sequence.transitions = {opening, dissolve, closing};
        std::string error;
        Check(fade.Validate(error),
              "the fade fixture is a valid document: " + error);
        ExpectList(fade, {"fondu_25.edl"},
                   "a fade from black, a dissolve and a fade to black");
        EdlExport exported;
        Check(Build(fade, exported, error) && exported.transitions == 3,
              "three transitions are counted");

        // A dip between two clips, or to a colour, has no CMX spelling.
        Document dip = fade;
        dip.sequence.transitions[1].type = "dip_to_color";
        Refused(dip, "between two clips", "a dip to colour mid-track");
        Document white = fade;
        white.sequence.transitions[2].color = "#ffffff";
        Refused(white, "black", "a fade to white");
        // The outgoing side of a dissolve, retimed, would need a second M2
        // line no reel could tell apart.
        Document retimed = fade;
        retimed.sequence.tracks[0].clips[0].speed = {1, 2};
        retimed.sequence.tracks[0].clips[0].duration = {100, 25};
        Refused(retimed, "M2 line speaks for",
                "a dissolve out of a retimed clip");
    }

    {
        Document speed;
        speed.sequence.name = "VITESSE";
        speed.sequence.frame_rate = k25;
        speed.library = {Media(kMediaA, "A.MP4", k25, 500),
                         Media(kMediaB, "B.MOV", k25, 500)};
        speed.sequence.tracks = {
            Track("01K70000000000000000000011", "video", 1,
                  {Clip("01K70000000000000000000021", kMediaB, k25, 10, 0, 20,
                        {2, 1})}),
            Track("01K70000000000000000000010", "video", 0,
                  {Clip("01K70000000000000000000020", kMediaA, k25, 0, 100, 50,
                        {1, 2})}),
            Track("01K70000000000000000000012", "audio", 0, {}),
        };
        ExpectList(speed, {"m2_25_V1.edl", "m2_25_V2.edl"},
                   "M2 at 12,5 and 50 i/s, one list per video track from the "
                   "bottom up");
        EdlExport exported;
        std::string error;
        Check(Build(speed, exported, error) && exported.retimed_clips == 2 &&
                  exported.files[0].track_id == "01K70000000000000000000010" &&
                  LossCount(exported, "audio_track",
                            "01K70000000000000000000012") == 1,
              "both speeds counted, V1 is the bottom track, the sound named");

        Document third = speed;
        third.sequence.tracks[1].clips[0].speed = {2, 3};
        Refused(third, "tenths of a frame per second",
                "2/3 at 25 i/s (16,666... i/s)");
        Document frozen = speed;
        frozen.sequence.tracks[1].clips[0].speed = {0, 1};
        Refused(frozen, "holds a frame", "a held frame");
    }

    // FilmCraft rounds a sub-frame position and warns; here it is refused.
    {
        Document offFrame = Cuts25();
        offFrame.sequence.tracks[0].clips[1].timeline_in = {201, 50};
        Refused(offFrame, "whole sequence frames", "a clip between frames");
        Document mixed = Cuts25();
        mixed.library[1].rate = {50, 1};
        Refused(mixed, "one timecode base", "a 50 i/s rush in a 25 i/s list");
        Document cadence = Cuts25();
        cadence.sequence.frame_rate = {25000, 1001};
        Refused(cadence, "no CMX 3600 timecode", "a cadence with no timecode");
        Document empty = Cuts25();
        empty.sequence.tracks[0].clips.clear();
        Refused(empty, "no video clip", "a timeline with no video clip");
        EdlExport exported;
        std::string error;
        Check(!Build(Cuts25(), exported, error, "01:00:00:30") &&
                  error.find("start timecode") != std::string::npos,
              "a start timecode the cadence cannot count is refused: " + error);
        Check(Build(Cuts25(), exported, error, "10:00:00:00") &&
                  exported.files[0].text.find(
                      "00:00:00:10 00:00:04:10 10:00:00:00 10:00:04:00") !=
                      std::string::npos,
              "the record side starts where it is asked to");
    }
}

void TestLosses() {
    Document document = Cuts25();
    DocumentClip& graded = document.sequence.tracks[0].clips[0];
    ClipEffect exposure;
    exposure.type = "color.exposure";
    exposure.params["amount"] = {1, 10};
    graded.effects = {exposure};
    graded.framing.scale = {3, 2};
    graded.opacity = {1, 2};
    // EF3 -- docs/roadmap/EFFETS-2026-10.md.
    graded.blur = {10, 1};
    // PA8 -- docs/roadmap/PARITE-2026-11.md.
    graded.mask.shape = kClipMaskEllipse;
    DocumentClip& ramped = document.sequence.tracks[0].clips[1];
    ClipKeyframe first;
    first.id = "01K700000000000000000000R1";
    first.time = {0, 25};
    first.value = {1, 1};
    ClipKeyframe last;
    last.id = "01K700000000000000000000R2";
    last.time = {50, 25};
    last.value = {2, 1};
    ramped.speed_ramp = {first, last};
    const Ulid gradedId = graded.id;
    const Ulid rampedId = ramped.id;
    DocumentClip motion;
    motion.id = "01K70000000000000000000022";
    motion.composition_id = "01K70000000000000000000040";
    motion.timeline_in = {150, 25};
    motion.duration = {25, 25};
    document.sequence.tracks[0].clips.push_back(motion);
    document.sequence.tracks.push_back(
        Track("01K70000000000000000000013", "caption", 2, {}));
    DocumentTrack hidden =
        Track("01K70000000000000000000014", "video", 3,
              {Clip("01K70000000000000000000023", kMediaA, k25, 0, 0, 10)});
    hidden.visible = false;
    document.sequence.tracks.push_back(hidden);
    document.sequence.look_lut.digest = "abc";
    document.sequence.look_lut.name = "look.cube";
    DocumentMarker marker;
    marker.time = {25, 25};
    document.sequence.markers = {marker};

    EdlExport exported;
    std::string error;
    Check(Build(document, exported, error), "losses do not refuse: " + error);
    Check(LossCount(exported, "grade", gradedId) == 1 &&
              LossCount(exported, "framing", gradedId) == 1 &&
              LossCount(exported, "opacity", gradedId) == 1 &&
              LossCount(exported, "blur", gradedId) == 1 &&
              LossCount(exported, "mask", gradedId) == 1 &&
              LossCount(exported, "speed_ramp", rampedId) == 1 &&
              LossCount(exported, "motion_clip", motion.id) == 1 &&
              LossCount(exported, "caption_track",
                        "01K70000000000000000000013") == 1 &&
              LossCount(exported, "hidden_track",
                        "01K70000000000000000000014") == 1 &&
              LossCount(exported, "look_lut") == 1 &&
              LossCount(exported, "markers") == 1 && exported.files.size() == 1,
          "every loss is named by what it is about");
    // The ramp reads 1 to 2 over its whole length: 3/2 on average, the
    // source span it covers, and M2 says 37.5 frames per second.
    Check(exported.files[0].text.find("M2   AX       037.5") !=
                  std::string::npos &&
              exported.files[0].text.find(
                  "00:00:00:00 00:00:02:00 01:00:04:00 01:00:06:00") !=
                  std::string::npos,
          "a ramp is listed at its mean speed, its cut points exact:\n" +
              exported.files[0].text);

    // A transition touching the ramp could not say what it reads.
    DocumentTransition closing;
    closing.id = "01K70000000000000000000033";
    closing.track_id = "01K70000000000000000000010";
    closing.left_clip_id = rampedId;
    closing.type = "dip_to_color";
    closing.duration = {5, 25};
    closing.alignment = TransitionAlignment::EndAtCut;
    document.sequence.transitions = {closing};
    document.sequence.tracks[0].clips.pop_back();
    Refused(document, "ramps its speed", "a fade out of a ramp");
}

// A hidden track keeps its number: the file of the window's V2 is _V2, even
// when V1 is hidden and writes no list.
void TestHiddenTrackNumbering() {
    Document document = Cuts25();
    document.sequence.tracks[0].visible = false;
    document.sequence.tracks.push_back(
        Track("01K70000000000000000000015", "video", 1,
              {Clip("01K70000000000000000000024", kMediaB, k25, 0, 0, 25)}));
    EdlExport exported;
    std::string error;
    Check(Build(document, exported, error) && exported.files.size() == 1 &&
              exported.files[0].number == 2 &&
              exported.files[0].text.find("TITLE: FILM V2") !=
                  std::string::npos &&
              LossCount(exported, "hidden_track",
                        "01K70000000000000000000010") == 1,
          "a hidden V1 writes no list and V2 keeps its number: " + error);
}

void TestPublication() {
    namespace fs = std::filesystem;
    const fs::path root =
        fs::temp_directory_path() / ("cutmachine-edl-" + GenerateUlid());
    fs::create_directories(root);
    Document document = Cuts25();
    document.sequence.tracks.push_back(
        Track("01K70000000000000000000011", "video", 1,
              {Clip("01K70000000000000000000022", kMediaA, k25, 0, 0, 25)}));
    std::string report;
    std::string errorName;
    std::string error;
    const std::string path = (root / "film.edl").string();
    Check(ExportEdlFiles(document, path, {}, report, errorName, error),
          "the lists are written: " + error);
    Check(fs::exists(root / "film_V1.edl") &&
              fs::exists(root / "film_V2.edl") && !fs::exists(path),
          "one file per video track, named after it");
    Check(report.find("\"verified_in_resolve\":false") != std::string::npos &&
              report.find("\"source_timecodes\":\"media_time_from_zero\"") !=
                  std::string::npos &&
              report.find("\"track\":\"V2\"") != std::string::npos,
          "the report says what it is: " + report);
    // Every list or none: V2 already there refuses the call, V1 untouched.
    fs::remove(root / "film_V1.edl");
    Check(!ExportEdlFiles(document, path, {}, report, errorName, error) &&
              errorName == "OutputExists" && !fs::exists(root / "film_V1.edl"),
          "an existing destination refuses the whole export: " + errorName);
    Check(!ExportEdlFiles(document, (root / "film.txt").string(), {}, report,
                          errorName, error) &&
              errorName == "ValidationFailed",
          "a path that is not a .edl is refused");
    fs::remove_all(root);
}

Document Marked() {
    Document document = Cuts25();
    DocumentMarker start;
    start.id = "01K70000000000000000000050";
    start.name = "Début";
    start.time = {5, 1};
    start.color = "#F5C542";
    DocumentMarker passage;
    passage.id = "01K70000000000000000000051";
    passage.name = "Passage";
    passage.time = {250, 25};
    passage.duration = {2, 1};
    passage.color = "#4da3ff";
    DocumentMarker question;
    question.id = "01K70000000000000000000052";
    question.name = "Question ?";
    question.time = {300, 25};
    question.color = "#ff8a3d";
    question.category = "question";
    // Out of order on purpose: the list is sorted by time.
    document.sequence.markers = {passage, start, question};
    return document;
}

void TestMarkerLists() {
    std::string error;
    EdlMarkerExport loc;
    Check(BuildEdlMarkers(Marked(), EdlMarkerStyle::Loc,
                          kEdlDefaultStartTimecode, loc, error),
          "LOC markers build: " + error);
    Check(loc.text == Fixture("markers_loc.edl"),
          "LOC markers match the fixture:\n" + loc.text);
    Check(loc.colors_approximated ==
                  std::vector<Ulid>{"01K70000000000000000000052"} &&
              loc.durations_not_sent ==
                  std::vector<Ulid>{"01K70000000000000000000051"},
          "the orange question and the span's length are the losses named");
    EdlMarkerExport resolve;
    Check(BuildEdlMarkers(Marked(), EdlMarkerStyle::Resolve,
                          kEdlDefaultStartTimecode, resolve, error),
          "Resolve markers build: " + error);
    Check(resolve.text == Fixture("markers_resolve.edl"),
          "Resolve markers match the fixture:\n" + resolve.text);
    Check(resolve.durations_not_sent.empty(),
          "Resolve's style keeps a span's length");

    Document piped = Marked();
    piped.sequence.markers[0].name = "a | b";
    Check(!BuildEdlMarkers(piped, EdlMarkerStyle::Resolve,
                           kEdlDefaultStartTimecode, resolve, error) &&
              error.find("'|'") != std::string::npos,
          "a '|' in a name is refused in Resolve's style: " + error);
    Check(BuildEdlMarkers(piped, EdlMarkerStyle::Loc, kEdlDefaultStartTimecode,
                          loc, error),
          "and allowed in a LOC comment");
    Document between = Marked();
    between.sequence.markers[0].time = {1, 50};
    Check(!BuildEdlMarkers(between, EdlMarkerStyle::Loc,
                           kEdlDefaultStartTimecode, loc, error) &&
              error.find("whole sequence frames") != std::string::npos,
          "a marker between frames is refused: " + error);
    Check(!BuildEdlMarkers(Cuts25(), EdlMarkerStyle::Loc,
                           kEdlDefaultStartTimecode, loc, error),
          "a timeline with no marker has no list");
}

void TestMarkerImport() {
    std::string error;
    // Written, then read back: the two styles against a timeline without
    // markers.
    for (const EdlMarkerStyle style :
         {EdlMarkerStyle::Loc, EdlMarkerStyle::Resolve}) {
        const bool isLoc = style == EdlMarkerStyle::Loc;
        const std::string what = isLoc ? "LOC" : "Resolve";
        EdlMarkerExport written;
        Check(BuildEdlMarkers(Marked(), style, kEdlDefaultStartTimecode,
                              written, error),
              what + " list builds: " + error);
        Document target = Cuts25();
        EdlMarkerImport imported;
        Check(BuildEdlMarkerImport(target, written.text,
                                   kEdlDefaultStartTimecode, imported, error),
              what + " list reads: " + error);
        Check(imported.operations.size() == 3 && imported.already_present == 0,
              what + ": three markers to add");
        const std::string before = target.SaveToString();
        EditLog log;
        EditError editError = EditError::None;
        std::string message;
        Operation operation = EdlMarkerImportOperation(imported);
        Check(std::holds_alternative<BatchOperation>(operation),
              what + ": several markers are one batch");
        Check(log.Apply(target, operation, editError, message),
              what + " import applies: " + message);
        Check(log.AppliedEntries().size() == 1,
              what + " import is one undo step");
        const auto& markers = target.sequence.markers;
        Check(markers.size() == 3, what + " import adds three markers");
        if (markers.size() == 3) {
            Check(markers[0].name == "Début" &&
                      markers[0].time.compare({125, 25}) == 0 &&
                      markers[0].duration.value == 0 &&
                      markers[0].color == "#f5c542" &&
                      markers[0].category == "note",
                  what + ": the point at 5 s comes back yellow");
            Check(markers[1].name == "Passage" &&
                      markers[1].time.compare({10, 1}) == 0 &&
                      markers[1].color == "#4da3ff" &&
                      markers[1].duration.compare(
                          isLoc ? RationalTime{0, 1} : RationalTime{2, 1}) == 0,
                  what + ": the span comes back blue, " +
                      (isLoc ? "as a point" : "two seconds long"));
            Check(markers[2].name == "Question ?" &&
                      markers[2].color == "#f5c542",
                  what +
                      ": the orange question comes back as the nearest "
                      "colour, yellow");
        }
        // Imported a second time: nothing doubles.
        EdlMarkerImport again;
        Check(BuildEdlMarkerImport(target, written.text,
                                   kEdlDefaultStartTimecode, again, error) &&
                  again.operations.empty() && again.already_present == 3,
              what + ": a list imported twice adds nothing");
        Check(log.Undo(target, editError, message) &&
                  target.SaveToString() == before,
              what + ": one undo restores the document byte for byte");
    }

    // A list as Resolve writes one at 29,97: CRLF, trailing spaces,
    // drop-frame, a span, and a LOC comment beside.
    {
        Document ntsc;
        ntsc.sequence.frame_rate = k2997;
        EdlMarkerImport imported;
        Check(BuildEdlMarkerImport(ntsc, Fixture("resolve_2997df_markers.edl"),
                                   kEdlDefaultStartTimecode, imported, error),
              "Resolve's 29,97 list reads: " + error);
        Check(imported.added.size() == 3, "three markers in Resolve's list");
        if (imported.added.size() == 3) {
            Check(imported.added[0].frame == 0 &&
                      imported.added[0].color == "#ff4d4d" &&
                      imported.added[0].name == "Raccord" &&
                      imported.added[0].duration_frames == 0,
                  "Red at the start, a point");
            Check(imported.added[1].frame == 1800 &&
                      imported.added[1].duration_frames == 15 &&
                      imported.added[1].color == "#8fe0b8" &&
                      imported.added[1].name == "Respiration",
                  "Mint at 01:01:00;02 is frame 1800, fifteen frames long");
            Check(imported.added[2].frame == 17982 &&
                      imported.added[2].color == "#4caf50",
                  "GREEN at the tenth minute is frame 17982");
        }
        Check(std::holds_alternative<BatchOperation>(
                  EdlMarkerImportOperation(imported)),
              "Resolve's three markers are one batch");
    }

    // A 29,97 list counted without drop-frame: the timeline's start reads in
    // the list's count, so its markers land where they were, not 108 frames
    // an hour later.
    {
        Document ntsc;
        ntsc.sequence.frame_rate = k2997;
        for (const char* list :
             {"FCM: NON-DROP FRAME\n* LOC: 01:00:05:00 RED x\n",
              "FCM: DROP FRAME\n* LOC: 01:00:05;00 RED x\n",
              "* LOC: 01:00:05;00 RED x\n"}) {
            EdlMarkerImport imported;
            Check(BuildEdlMarkerImport(ntsc, list, kEdlDefaultStartTimecode,
                                       imported, error) &&
                      imported.added.size() == 1 &&
                      imported.added[0].frame == 150,
                  std::string("five seconds in, whatever the count: ") + list +
                      error);
        }
    }

    // A marker the list leaves unnamed takes the window's default name.
    {
        Document target = Cuts25();
        EdlMarkerImport imported;
        Check(BuildEdlMarkerImport(
                  target,
                  "* LOC: 01:00:01:00 RED\n"
                  "001  001      V     C        01:00:02:00 01:00:02:01 "
                  "01:00:02:00 01:00:02:01\n |C:ResolveColorBlue |M: |D:1\n",
                  kEdlDefaultStartTimecode, imported, error) &&
                  imported.added.size() == 2 &&
                  imported.added[0].name == kTimelineMarkerDefaultName &&
                  imported.added[1].name == kTimelineMarkerDefaultName,
              "unnamed markers are named as the window names one: " + error);
    }

    // Refusals, by line.
    {
        Document target = Cuts25();
        EdlMarkerImport imported;
        const std::string orange =
            "001  001      V     C        01:00:05:00 01:00:05:01 01:00:05:00 "
            "01:00:05:01\n |C:ResolveColorOrange |M:x |D:1\n";
        Check(!BuildEdlMarkerImport(target, orange, kEdlDefaultStartTimecode,
                                    imported, error) &&
                  error.find("line 2") != std::string::npos &&
                  error.find("'Orange'") != std::string::npos,
              "an unknown colour is refused by line, no default guessed: " +
                  error);
        Check(
            !BuildEdlMarkerImport(target, "* LOC: 01:00:05:00 note\n",
                                  kEdlDefaultStartTimecode, imported, error) &&
                error.find("'note'") != std::string::npos,
            "a LOC with no colour word is refused: " + error);
        Check(
            !BuildEdlMarkerImport(target, "* LOC: 00:59:59:00 RED early\n",
                                  kEdlDefaultStartTimecode, imported, error) &&
                error.find("before the timeline's start") != std::string::npos,
            "a marker before the start timecode is refused: " + error);
        Check(
            !BuildEdlMarkerImport(target,
                                  "FCM: DROP FRAME\n* LOC: 01:00:05;00 RED "
                                  "x\n",
                                  kEdlDefaultStartTimecode, imported, error) &&
                error.find("drop-frame") != std::string::npos,
            "a drop-frame list on a 25 i/s timeline is refused: " + error);
        Check(
            !BuildEdlMarkerImport(target, " |C:ResolveColorRed |M:x |D:1\n",
                                  kEdlDefaultStartTimecode, imported, error) &&
                error.find("under no event") != std::string::npos,
            "a Resolve marker line under no event is refused");
        // 27 frames do not exist at 25 i/s: the marker under that event is
        // refused there, not put on the event before.
        Check(!BuildEdlMarkerImport(
                  target,
                  "001  001      V     C        01:00:05:00 01:00:05:01 "
                  "01:00:05:00 01:00:05:01\n |C:ResolveColorRed |M:a |D:1\n"
                  "002  001      V     C        01:00:10:27 01:00:10:28 "
                  "01:00:10:27 01:00:10:28\n |C:ResolveColorRed |M:b |D:1\n",
                  kEdlDefaultStartTimecode, imported, error) &&
                  error.find("line 3") != std::string::npos,
              "an event whose record in does not read is refused by line: " +
                  error);
        Check(
            !BuildEdlMarkerImport(target, "* LOC: 01:00:05:00 RED a\tb\n",
                                  kEdlDefaultStartTimecode, imported, error) &&
                error.find("line 1") != std::string::npos,
            "a control character in a name is refused by line: " + error);
        Check(
            !BuildEdlMarkerImport(target, Fixture("cut_25.edl"),
                                  kEdlDefaultStartTimecode, imported, error) &&
                error.find("no marker") != std::string::npos,
            "a list with no marker is refused");
        Check(BuildEdlMarkerImport(target, "* LOC: 10:00:05:00 red x\n",
                                   "10:00:00:00", imported, error) &&
                  imported.added.size() == 1 &&
                  imported.added[0].frame == 125 &&
                  std::holds_alternative<AddMarkerOperation>(
                      EdlMarkerImportOperation(imported)),
              "another start timecode, a lower-case colour, a single "
              "operation: " +
                  error);
    }
}

}  // namespace

int main() {
    TestTimecodes();
    TestLists();
    TestLosses();
    TestHiddenTrackNumbering();
    TestPublication();
    TestMarkerLists();
    TestMarkerImport();
    if (failures != 0) {
        std::cerr << failures << " EDL check(s) failed\n";
        return 1;
    }
    std::cout << "EDL tests passed\n";
    return 0;
}
