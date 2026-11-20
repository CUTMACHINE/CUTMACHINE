// OR4 -- docs/roadmap/DONNEES-2026-10.md. A montage checked against a named
// policy: what the policy reads as, how the end of the film is composed
// frame by frame, and the verdict on file measures handed in. Portable: no
// AppKit, no Metal, and FFmpeg is never run here (the file is measured in
// the MCP and CLI suites).

#include "TimelineCheck.h"

#include "Json.h"
#include "MotionTemplates.h"
#include "TransitionRender.h"

#include <iostream>
#include <string>
#include <vector>

#include "TestSupport.h"

namespace {

using mcp_json::Value;

const Ulid kSource = "01KA0000000000000000000001";
const Ulid kPictureTrack = "01KA0000000000000000000002";
const Ulid kSoundTrack = "01KA0000000000000000000003";
const Ulid kFirst = "01KA0000000000000000000004";
const Ulid kSecond = "01KA0000000000000000000005";
const Ulid kFirstSound = "01KA0000000000000000000006";
const Ulid kSecondSound = "01KA0000000000000000000007";
const Ulid kLink = "01KA0000000000000000000008";
const Ulid kSequence = "01KA0000000000000000000009";
const Ulid kTransition = "01KA000000000000000000000A";

// An interview cut in two: two 100-frame shots of one rush on V1, their
// sound under them on A1, all four in one link group, in sync. A film of
// 200 frames at 25 i/s.
Document Interview() {
    Document document;
    document.sequence.id = kSequence;
    document.sequence.frame_rate = MediaRate{25, 1};
    DocumentSource source;
    source.id = kSource;
    source.path = "itw.mov";
    source.rate = MediaRate{25, 1};
    source.duration = RationalTime(1000, 25);
    document.sources.push_back(source);

    DocumentTrack picture;
    picture.id = kPictureTrack;
    picture.kind = "video";
    picture.index = 0;
    DocumentClip first;
    first.id = kFirst;
    first.source_id = kSource;
    first.source_in = RationalTime(100, 25);
    first.duration = RationalTime(100, 25);
    first.timeline_in = RationalTime(0, 25);
    first.link_group_id = kLink;
    DocumentClip second = first;
    second.id = kSecond;
    second.source_in = RationalTime(400, 25);
    second.timeline_in = RationalTime(100, 25);
    picture.clips = {first, second};

    DocumentTrack sound;
    sound.id = kSoundTrack;
    sound.kind = "audio";
    sound.index = 1;
    DocumentClip firstSound = first;
    firstSound.id = kFirstSound;
    DocumentClip secondSound = second;
    secondSound.id = kSecondSound;
    sound.clips = {firstSound, secondSound};

    document.sequence.tracks = {picture, sound};
    return document;
}

const TimelineCheckResult* Find(const TimelineCheckReport& report,
                                const std::string& name) {
    for (const TimelineCheckResult& check : report.checks) {
        if (check.name == name) {
            return &check;
        }
    }
    return nullptr;
}

TimelineCheckStatus StatusOf(const TimelineCheckReport& report,
                             const std::string& name) {
    const TimelineCheckResult* check = Find(report, name);
    if (check == nullptr) {
        throw std::runtime_error("no check " + name);
    }
    return check->status;
}

TimelineCheckReport Run(const Document& document,
                        const TimelinePolicy& policy = TimelinePolicy{},
                        const TimelineFileMeasures* file = nullptr) {
    TimelineCheckReport report;
    std::string error;
    if (!CheckTimeline(document, policy, file, report, error)) {
        throw std::runtime_error("CheckTimeline refused: " + error);
    }
    return report;
}

TimelinePolicy Parse(const std::string& json) {
    Value value;
    std::string error;
    if (!Value::Parse(json, value, error)) {
        throw std::runtime_error("bad test JSON: " + error);
    }
    TimelinePolicy policy;
    if (!ParseTimelinePolicy(value, policy, error)) {
        throw std::runtime_error("policy refused: " + error);
    }
    return policy;
}

std::string Refusal(const std::string& json) {
    Value value;
    std::string error;
    if (!Value::Parse(json, value, error)) {
        return "bad test JSON";
    }
    TimelinePolicy policy;
    return ParseTimelinePolicy(value, policy, error) ? std::string() : error;
}

DocumentTransition Closing(int64_t frames, const std::string& color) {
    DocumentTransition transition;
    transition.id = kTransition;
    transition.track_id = kPictureTrack;
    transition.left_clip_id = kSecond;
    transition.type = kTransitionDipToColor;
    transition.color = color;
    transition.duration = RationalTime(frames, 25);
    transition.alignment = TransitionAlignment::EndAtCut;
    return transition;
}

// DEC6 -- docs/roadmap/MOTION-BIBLIOTHEQUE-2026-10.md. A brand's charter,
// as the library would give it: blue and white, one font, captions.
MotionKitInfo BlueCharter() {
    MotionKitInfo kit;
    std::string error;
    if (!ParseMotionKit(
            "bleu",
            R"({"format":"brand-kit.v1","description":"Charte bleue.",)"
            R"("props":{"accent":"#0000FF","texte":"#FFFFFF",)"
            R"("fond":"#0000FF","police":"Avenir Next"},)"
            R"("captions":{"font_family":"Avenir Next","font_size":64,)"
            R"("color":"#FFFFFF","background_color":"#0000FF@0.8"}})",
            kit, error)) {
        throw std::runtime_error("charter refused: " + error);
    }
    return kit;
}

const Ulid kCaptionTrack = "01KA000000000000000000000B";
const Ulid kCarton = "01KA000000000000000000000C";
const Ulid kStyle = "01KA000000000000000000000D";
const Ulid kTitleTrack = "01KA000000000000000000000E";
const Ulid kTitle = "01KA000000000000000000000F";
const Ulid kComposition = "01KA000000000000000000000G";

// The interview with a carton in the charter's caption style and a card
// over its first two seconds: a blue fill, a white title in Avenir Next, a
// box that fades from blue to white (the mixes are nobody's choice), and a
// red box the instance hides.
Document Branded() {
    Document document = Interview();
    CaptionStyle style;
    style.id = kStyle;
    style.font_family = "Avenir Next";
    style.font_size = 64;
    style.color = "#FFFFFF";
    style.background_color = "#0000FF@0.8";
    document.sequence.caption_styles = {style};
    DocumentTrack captions;
    captions.id = kCaptionTrack;
    captions.kind = "caption";
    captions.index = 2;
    DocumentClip carton;
    carton.id = kCarton;
    carton.timeline_in = RationalTime(0, 25);
    carton.duration = RationalTime(50, 25);
    carton.caption_group_id = kStyle;
    carton.caption_text = "Bonjour";
    captions.clips = {carton};
    DocumentTrack titles;
    titles.id = kTitleTrack;
    titles.kind = "video";
    titles.index = 3;
    DocumentClip title;
    title.id = kTitle;
    title.composition_id = kComposition;
    title.source_in = RationalTime(0, 25);
    title.timeline_in = RationalTime(0, 25);
    title.duration = RationalTime(50, 25);
    titles.clips = {title};
    document.sequence.tracks.push_back(captions);
    document.sequence.tracks.push_back(titles);
    DocumentComposition composition;
    composition.id = kComposition;
    composition.name = "carton";
    std::string error;
    if (!Value::Parse(
            R"({"width":1080,"height":1920,"fps":25,"duration":50,)"
            R"("props":{"fond":{"type":"color","default":"#0000FF"},)"
            R"("texte":{"type":"color","default":"#FFFFFF"},)"
            R"("police":{"type":"text","default":"Avenir Next"},)"
            R"("alerte":{"type":"bool","default":false}},)"
            R"("root":{"type":"fill","style":{"backgroundColor":)"
            R"({"prop":"fond"}},"children":[)"
            R"({"type":"text","id":"titre","content":"Titre","style":)"
            R"({"fontFamily":{"prop":"police"},"color":{"prop":"texte"}}},)"
            R"({"type":"box","id":"fondu","animate":{"backgroundColor":)"
            R"({"interpolate":{"frames":[0,10],)"
            R"("values":["#0000FF","#FFFFFF"]}}}},)"
            R"({"type":"box","id":"alerte","visible":{"prop":"alerte"},)"
            R"("style":{"backgroundColor":"#FF0000"}}]}})",
            composition.definition, error)) {
        throw std::runtime_error("bad composition JSON: " + error);
    }
    document.compositions = {composition};
    return document;
}

TimelinePolicy WithCharter() {
    TimelinePolicy policy = Parse(R"({"kit":"bleu"})");
    policy.kit = BlueCharter();
    return policy;
}

}  // namespace

int main() {
    Test("DEC6: a policy names a charter by its library name", [] {
        const TimelinePolicy policy = Parse(R"({"kit":"bleu"})");
        Check(
            policy.kit_name == "bleu" && policy.name == "custom" && !policy.kit,
            "the name is read, the charter is the caller's to look up");
        Check(Refusal(R"({"kit":"../bleu"})")
                      .find("'policy.kit' must be the name of a charter") !=
                  std::string::npos,
              "a path is not a charter's name");
        TimelineCheckReport report;
        std::string error;
        Check(
            !CheckTimeline(Interview(), policy, nullptr, report, error) &&
                error.find("was not looked up") != std::string::npos,
            "a name left unresolved is refused, never judged empty: " + error);
        Check(StatusOf(Run(Interview()), "charter") ==
                  TimelineCheckStatus::NotRun,
              "without a kit, the charter check says it did not run");
    });

    Test("DEC6: an export in the charter passes it", [] {
        const TimelineCheckReport report = Run(Branded(), WithCharter());
        const TimelineCheckResult* charter = Find(report, "charter");
        const std::string json = SerializeTimelineCheck(report);
        Check(charter != nullptr &&
                  charter->status == TimelineCheckStatus::Passed,
              "the charter's own style and piece pass: " + json);
        Check(json.find(R"("kit":{"name":"bleu","colors":["#0000ff",)"
                        R"("#ffffff"],"fonts":["Avenir Next"]},)"
                        R"("caption_styles":1,"motion_clips":1,)"
                        R"("outside_values":[],"outside":[])") !=
                  std::string::npos,
              "what was allowed and what was read: " + json);
        Check(json.find(R"("policy":{"name":"custom",)") != std::string::npos &&
                  json.find(R"("kit":"bleu"})") != std::string::npos,
              "the report names the charter it judged against: " + json);
    });

    Test("DEC6: a colour or a font outside the charter is named", [] {
        Document document = Branded();
        document.sequence.caption_styles[0].color = "#FF0000";
        document.sequence.caption_styles[0].outline_width = 4;
        document.sequence.caption_styles[0].outline_color = "black";
        document.sequence.tracks.back().clips[0].motion_props =
            Value::MakeObject();
        document.sequence.tracks.back().clips[0].motion_props.Set(
            "police", Value::MakeString("Comic Sans MS"));
        const TimelineCheckReport report = Run(document, WithCharter());
        const std::string json = SerializeTimelineCheck(report);
        Check(StatusOf(report, "charter") == TimelineCheckStatus::Failed &&
                  !report.Passed(),
              "outside the charter fails the export: " + json);
        Check(json.find(R"({"in":"caption_style","style_id":")" + kStyle +
                        R"(","setting":"color","value":"#FF0000",)"
                        R"("cartons":1,"first_clip_id":")" +
                        kCarton + "\"}") != std::string::npos,
              "the caption colour, with its style and a carton: " + json);
        Check(json.find(R"("setting":"outline_color","value":"black")") !=
                  std::string::npos,
              "an outline drawn in black, which the charter lacks: " + json);
        Check(json.find(R"({"in":"motion","clip_id":")" + kTitle +
                        R"(","composition_id":")" + kComposition +
                        R"(","node":"titre","property":"fontFamily",)"
                        R"("value":"Comic Sans MS"})") != std::string::npos,
              "the font an instance binds, with its clip and node: " + json);
        Check(json.find("#FF0000\"}") == std::string::npos &&
                  json.find("\"node\":\"alerte\"") == std::string::npos &&
                  json.find("\"node\":\"fondu\"") == std::string::npos,
              "a hidden node and a fade's mixes are not findings: " + json);
        Check(json.find("\"3 font(s) or colour(s) outside the charter "
                        "'bleu', in 3 place(s)") != std::string::npos,
              "the reason counts them: " + json);
        Check(json.find(R"("outside_values":[{"value":"#FF0000","places":1},)"
                        R"({"value":"black","places":1},)"
                        R"({"value":"Comic Sans MS","places":1}])") !=
                  std::string::npos,
              "each value once, with the places that write it: " + json);
    });

    Test("DEC6: what nobody set is the evaluator's default, and judged", [] {
        Document document = Branded();
        Value& definition = document.compositions[0].definition;
        std::string error;
        Check(Value::Parse(
                  R"({"width":1080,"height":1920,"fps":25,"duration":50,)"
                  R"("root":{"type":"fill","children":[)"
                  R"({"type":"text","id":"nu","content":"Titre"}]}})",
                  definition, error),
              "a bare text: " + error);
        const std::string json =
            SerializeTimelineCheck(Run(document, WithCharter()));
        Check(
            json.find(R"("node":"nu","property":"color","value":"#000000")") !=
                    std::string::npos &&
                json.find(R"("node":"nu","property":"fontFamily",)"
                          R"("value":"system-ui")") != std::string::npos,
            "a text with no colour is black, with no family system-ui: " +
                json);
    });

    Test("a clean interview passes, and says what it did not run", [] {
        const Document document = Interview();
        std::string error;
        Check(document.Validate(error), "fixture validates: " + error);
        const TimelineCheckReport report = Run(document);
        Check(report.Passed(), "nothing fails");
        Check(
            StatusOf(report, "links") == TimelineCheckStatus::Passed &&
                StatusOf(report, "sync") == TimelineCheckStatus::Passed &&
                StatusOf(report, "final_black") == TimelineCheckStatus::Passed,
            "links, sync and the end are judged on the document");
        Check(StatusOf(report, "duration") == TimelineCheckStatus::NotRun &&
                  StatusOf(report, "caption_contrast") ==
                      TimelineCheckStatus::NotRun,
              "no bounds and no file: two checks not run, not passed");
        const std::string json = SerializeTimelineCheck(report);
        Check(json.find("\"policy\":{\"name\":\"default\","
                        "\"final_black\":\"forbidden\","
                        "\"jump_cuts\":\"allowed\","
                        "\"minimum_contrast\":{\"num\":3,\"den\":1}}") !=
                  std::string::npos,
              "the report names the default policy: " + json);
        Check(json.find("\"passed\":true,\"failed\":[],\"not_run\":["
                        "\"duration\",\"caption_contrast\",\"charter\","
                        "\"jump_cuts\"]") != std::string::npos,
              "verdict, failures and checks not run up front: " + json);
        Check(json.find("{\"name\":\"links\",\"status\":\"passed\","
                        "\"pairs\":2,\"unlinked\":[]}") != std::string::npos,
              "each picture is paired with the sound it overlaps: " + json);
        Check(json.find("{\"name\":\"final_black\",\"status\":\"passed\","
                        "\"from_frame\":175,\"until_frame\":200,"
                        "\"document\":[]}") != std::string::npos,
              "the last second, 25 frames, has no black: " + json);
        Check(report.generated.empty() &&
                  json.find("\"generated\":[]") != std::string::npos,
              "nothing generated, said as an empty list: " + json);
    });

    // DEC36 -- docs/roadmap/GENERATIF-2026-10.md. The clips of a generated
    // media are listed for the agent's report, in track order, and fail
    // nothing.
    Test("a generated media's clips are listed, and judged by no check", [] {
        Document document = Interview();
        LibraryMedia media;
        media.id = kSource;
        media.path = "generated/itw.mp4";
        media.filename = "itw.mp4";
        media.rate = MediaRate{25, 1};
        media.duration = RationalTime(1000, 25);
        media.generation.service = "fal.ai";
        media.generation.model = "fal-ai/bria/expand";
        media.generation.kind = "expand_image";
        media.generation.created_at = "2026-10-24T01:02:03Z";
        document.library.push_back(media);
        const TimelineCheckReport report = Run(document);
        Check(report.Passed(), "a generated shot is no fault");
        Check(report.generated.size() == 4 &&
                  report.generated[0].clip_id == kFirst &&
                  report.generated[1].clip_id == kSecond &&
                  report.generated[2].clip_id == kFirstSound &&
                  report.generated[0].track_id == kPictureTrack &&
                  report.generated[1].timeline_in == RationalTime(100, 25) &&
                  report.generated[0].generation.model == "fal-ai/bria/expand",
              "every clip of the media, picture then sound");
        const std::string json = SerializeTimelineCheck(report);
        Check(json.find("\"generated\":[{\"clip_id\":\"" + kFirst +
                        "\",\"track_id\":\"" + kPictureTrack +
                        "\",\"media_id\":\"" + kSource +
                        "\",\"timeline_in\":{\"value\":0,\"rate\":25},"
                        "\"duration\":{\"value\":100,\"rate\":25},"
                        "\"service\":\"fal.ai\",\"model\":"
                        "\"fal-ai/bria/expand\",\"kind\":\"expand_image\","
                        "\"created_at\":\"2026-10-24T01:02:03Z\"}") !=
                      std::string::npos &&
                  json.find("\"passed\":true") != std::string::npos,
              "the report lists them: " + json);
    });

    Test("the policy reads as written, and refuses what it cannot", [] {
        Check(Parse("{}").name == "default", "an empty policy is the default");
        const TimelinePolicy bounded = Parse(
            "{\"duration_max\":{\"value\":60,\"rate\":1},"
            "\"final_black\":\"allowed\","
            "\"minimum_contrast\":{\"num\":9,\"den\":2}}");
        Check(bounded.name == "custom" && bounded.duration_max &&
                  *bounded.duration_max == RationalTime(60, 1) &&
                  bounded.final_black == FinalBlackRule::Allowed &&
                  bounded.minimum_contrast_num == 9 &&
                  bounded.minimum_contrast_den == 2,
              "a changed policy without a name is called custom");
        Check(Parse("{\"name\":\"reel 9:16\",\"final_black\":\"allowed\"}")
                      .name == "reel 9:16",
              "a name given is kept");
        Check(Parse("{\"name\":\"itw\"}").name == "itw",
              "even on the default values");
        Check(
            Refusal("{\"black\":\"no\"}") == "'policy' has unknown key 'black'",
            "an unknown key is refused by name");
        Check(Refusal("{\"duration_min\":{\"value\":30,\"rate\":1},"
                      "\"duration_max\":{\"value\":20,\"rate\":1}}") ==
                  "'policy.duration_min' is longer than "
                  "'policy.duration_max'",
              "bounds that cross");
        Check(Refusal("{\"duration_min\":{\"value\":-1,\"rate\":1}}") ==
                  "'policy.duration_min' must not be negative",
              "a negative duration");
        Check(Refusal("{\"final_black\":\"maybe\"}") ==
                  "'policy.final_black' must be \"forbidden\" or "
                  "\"allowed\"",
              "an unknown rule");
        Check(Refusal("{\"minimum_contrast\":{\"num\":1,\"den\":2}}") ==
                  "'policy.minimum_contrast' must lie between 1 and 21",
              "a contrast under one");
        Check(Refusal("{\"minimum_contrast\":3}") ==
                  "'policy.minimum_contrast' must be {\"num\",\"den\"}",
              "a contrast that is not a fraction");
        Check(Refusal("{\"final_window\":{\"value\":0,\"rate\":25}}") ==
                  "'policy.final_window' must be longer than zero",
              "an empty window");
        Check(Refusal("[]") == "'policy' must be a JSON object",
              "a policy that is not an object");
    });

    Test("picture and sound of one source, unlinked, are named", [] {
        Document document = Interview();
        document.sequence.tracks[1].clips[1].link_group_id.clear();
        const TimelineCheckReport report = Run(document);
        Check(!report.Passed() &&
                  StatusOf(report, "links") == TimelineCheckStatus::Failed,
              "an unlinked pair fails links");
        const std::string json = SerializeTimelineCheck(report);
        Check(json.find("\"unlinked\":[{\"video_clip_id\":\"" + kSecond +
                        "\",\"audio_clip_id\":\"" + kSecondSound +
                        "\",\"drift\":{\"value\":0,\"rate\":25}}]") !=
                  std::string::npos,
              "the pair is named with its drift, zero here: " + json);
        Check(json.find("\"failed\":[\"links\"]") != std::string::npos,
              "listed among the failures");

        // A muted track plays nothing: its clips pair with no picture.
        document.sequence.tracks[1].muted = true;
        Check(StatusOf(Run(document), "links") == TimelineCheckStatus::Passed,
              "a muted track is not delivered");
    });

    Test("a linked pair out of sync fails sync", [] {
        Document document = Interview();
        document.sequence.tracks[1].clips[1].source_in = RationalTime(402, 25);
        const TimelineCheckReport report = Run(document);
        Check(StatusOf(report, "sync") == TimelineCheckStatus::Failed &&
                  StatusOf(report, "links") == TimelineCheckStatus::Passed,
              "linked but two frames apart");
        const std::string json = SerializeTimelineCheck(report);
        Check(json.find("\"out_of_sync\":[{\"video_clip_id\":\"" + kSecond +
                        "\",\"audio_clip_id\":\"" + kSecondSound +
                        "\",\"drift\":{\"value\":-2,\"rate\":25}}]") !=
                  std::string::npos,
              "TimelineStats' drift, exact: " + json);
    });

    Test("the duration against the policy's bounds", [] {
        const Document document = Interview();
        Check(StatusOf(Run(document, Parse("{\"duration_max\":"
                                           "{\"value\":8,\"rate\":1}}")),
                       "duration") == TimelineCheckStatus::Passed,
              "eight seconds is not over eight");
        const TimelineCheckReport tooShort = Run(
            document, Parse("{\"duration_min\":{\"value\":201,\"rate\":25}}"));
        Check(StatusOf(tooShort, "duration") == TimelineCheckStatus::Failed,
              "one frame short of the minimum fails");
        Check(
            SerializeTimelineCheck(tooShort).find(
                "\"reason\":\"the timeline is shorter than duration_min\","
                "\"duration\":{\"value\":200,\"rate\":25},"
                "\"minimum\":{\"value\":201,\"rate\":25}") != std::string::npos,
            "with the duration and the bound");
        Check(StatusOf(Run(document, Parse("{\"duration_max\":"
                                           "{\"value\":199,\"rate\":25}}")),
                       "duration") == TimelineCheckStatus::Failed,
              "one frame over the maximum fails");
    });

    Test("a closing fade to black is a fade, frame by frame", [] {
        Document document = Interview();
        document.sequence.transitions = {Closing(50, "#000000")};
        std::string error;
        Check(document.Validate(error), "the closing validates: " + error);
        std::vector<EndingFrame> frames;
        Check(ClassifyEndingFrames(document, 148, 52, frames, error),
              "classified: " + error);
        // The veil starts at frame 150 at zero and rises to 49/50 on the
        // last frame: nearly, not quite, black (TransitionRender.h).
        Check(frames.size() == 52 && frames[0] == EndingFrame::Picture &&
                  frames[1] == EndingFrame::Picture &&
                  frames[2] == EndingFrame::Picture &&
                  frames[3] == EndingFrame::Fading &&
                  frames[51] == EndingFrame::Fading,
              "picture up to the veil's first visible frame, then a fade");
        const TimelineCheckReport report = Run(document);
        Check(StatusOf(report, "final_black") == TimelineCheckStatus::Failed,
              "the default policy forbids it");
        const std::string json = SerializeTimelineCheck(report);
        Check(json.find("\"document\":[{\"from_frame\":175,\"until_frame\":200,"
                        "\"kind\":\"fading\",\"cause\":\"dip_to_color\","
                        "\"transition_id\":\"" +
                        kTransition + "\"}]") != std::string::npos,
              "the run names the transition: " + json);
        Check(StatusOf(Run(document, Parse("{\"final_black\":\"allowed\"}")),
                       "final_black") == TimelineCheckStatus::Passed,
              "a policy that allows it passes, with the same evidence");
        const TimelineCheckReport wide =
            Run(document, Parse("{\"final_window\":{\"value\":3,\"rate\":1}}"));
        Check(
            SerializeTimelineCheck(wide).find(
                "\"from_frame\":125,\"until_frame\":200,\"document\":[{"
                "\"from_frame\":151,\"until_frame\":200,") != std::string::npos,
            "a wider window sees where the fade begins");
    });

    Test("a dip through white ends on white, not on black", [] {
        Document document = Interview();
        document.sequence.transitions = {Closing(50, "#ffffff")};
        Check(StatusOf(Run(document), "final_black") ==
                  TimelineCheckStatus::Passed,
              "not black");
    });

    Test("sound that outlasts the picture leaves black at the end", [] {
        Document document = Interview();
        document.sequence.tracks[1].clips[1].duration = RationalTime(110, 25);
        const TimelineCheckReport report = Run(document);
        Check(StatusOf(report, "final_black") == TimelineCheckStatus::Failed,
              "ten frames of nothing");
        Check(SerializeTimelineCheck(report).find(
                  "\"from_frame\":185,\"until_frame\":210,\"document\":[{"
                  "\"from_frame\":200,\"until_frame\":210,\"kind\":\"black\","
                  "\"cause\":\"nothing_on_screen\"}]") != std::string::npos,
              "the composite's black, named as nothing on screen");
    });

    Test("a clip that fades out over nothing is a fade to black", [] {
        Document document = Interview();
        DocumentClip& last = document.sequence.tracks[0].clips[1];
        ClipParameterAnimation fade;
        fade.parameter = ClipAnimatedParameter::Opacity;
        ClipKeyframe from;
        from.id = "01KA000000000000000000000B";
        from.time = RationalTime(90, 25);
        from.value = EffectParamValue{1, 1};
        ClipKeyframe to = from;
        to.id = "01KA000000000000000000000C";
        to.time = RationalTime(99, 25);
        to.value = EffectParamValue{0, 1};
        fade.keyframes = {from, to};
        last.animation = {fade};
        std::string error;
        Check(document.Validate(error), "the fade validates: " + error);
        std::vector<EndingFrame> frames;
        Check(ClassifyEndingFrames(document, 189, 11, frames, error),
              "classified: " + error);
        Check(frames.size() == 11 && frames[0] == EndingFrame::Picture &&
                  frames[1] == EndingFrame::Picture &&
                  frames[2] == EndingFrame::Fading &&
                  frames[9] == EndingFrame::Fading &&
                  frames[10] == EndingFrame::Black,
              "full at its first key, fading after, nothing at zero");
        const std::string json = SerializeTimelineCheck(Run(document));
        Check(json.find("{\"from_frame\":191,\"until_frame\":199,"
                        "\"kind\":\"fading\",\"cause\":\"clip_opacity\","
                        "\"clip_id\":\"" +
                        kSecond +
                        "\"},{\"from_frame\":199,\"until_frame\":200,"
                        "\"kind\":\"black\",\"cause\":\"clip_opacity\","
                        "\"clip_id\":\"" +
                        kSecond + "\"}") != std::string::npos,
              "both runs name the clip: " + json);

        // The same fade over a picture on V0 underneath is a blend of two
        // pictures, never black.
        DocumentTrack under;
        under.id = "01KA000000000000000000000D";
        under.kind = "video";
        under.index = -1;
        DocumentClip plate = document.sequence.tracks[0].clips[0];
        plate.id = "01KA000000000000000000000E";
        plate.link_group_id.clear();
        plate.duration = RationalTime(200, 25);
        under.clips = {plate};
        document.sequence.tracks.push_back(under);
        Check(StatusOf(Run(document), "final_black") ==
                  TimelineCheckStatus::Passed,
              "a picture underneath shows through");
    });

    Test("the window is whole frames, or refused", [] {
        const Document document = Interview();
        TimelineCheckReport report;
        std::string error;
        Check(!CheckTimeline(document,
                             Parse("{\"final_window\":{\"value\":1,"
                                   "\"rate\":30}}"),
                             nullptr, report, error) &&
                  error ==
                      "'policy.final_window' must be a whole number of "
                      "sequence frames",
              "a thirtieth of a second at 25 i/s is refused: " + error);
        int64_t frames = 0;
        DocumentSequence ntsc;
        ntsc.frame_rate = MediaRate{30000, 1001};
        Check(FinalWindowFrames(ntsc, TimelinePolicy{}, frames, error) &&
                  frames == 30,
              "one second at 29.97 rounds up to 30 frames");
    });

    Test("the delivered file is judged too", [] {
        const Document document = Interview();
        TimelineFileMeasures file;
        file.final_frames_black.assign(25, false);
        file.final_frames_black[23] = true;
        file.final_frames_black[24] = true;
        CaptionLegibility legible;
        legible.clip_id = "01KA000000000000000000000F";
        legible.text = "bonjour";
        legible.frame = 40;
        legible.ring_pixels = 120;
        legible.contrast_median = 12.0;
        legible.contrast_low = 9.5;
        CaptionLegibility faint = legible;
        faint.clip_id = "01KA000000000000000000000G";
        faint.text = "chemise blanche";
        faint.contrast_median = 2.104;
        faint.contrast_low = 1.6;
        file.legibility = {legible, faint};
        const TimelineCheckReport report =
            Run(document, TimelinePolicy{}, &file);
        Check(StatusOf(report, "final_black") == TimelineCheckStatus::Failed,
              "black in the file fails though the document has none");
        Check(
            StatusOf(report, "caption_contrast") == TimelineCheckStatus::Failed,
            "one carton under 3:1");
        const std::string json = SerializeTimelineCheck(report);
        Check(json.find("\"file_measured\":true") != std::string::npos,
              "the report says a file was measured");
        Check(json.find("\"document\":[],\"file_frames\":25,\"file\":[{"
                        "\"from_frame\":198,\"until_frame\":200}]") !=
                  std::string::npos,
              "the file's black frames, aligned on the film's end: " + json);
        Check(json.find(
                  "\"cartons\":2,\"below_minimum\":[{\"clip_id\":\"" +
                  faint.clip_id +
                  "\",\"text\":\"chemise blanche\",\"frame\":40,"
                  "\"contrast_median\":2.100000,\"contrast_low\":1.600000}]") !=
                  std::string::npos,
              "only the faint carton is listed: " + json);

        TimelineFileMeasures unmeasured = file;
        unmeasured.final_frames_black.assign(25, false);
        unmeasured.legibility.clear();
        unmeasured.legibility_error = "no caption renderer";
        const TimelineCheckReport partial =
            Run(document, TimelinePolicy{}, &unmeasured);
        Check(partial.Passed() &&
                  StatusOf(partial, "caption_contrast") ==
                      TimelineCheckStatus::NotRun &&
                  Find(partial, "caption_contrast")->reason ==
                      "no caption renderer",
              "cartons that could not be measured are not run, with why");
    });

    Test("a grey frame is black as blackdetect sees it", [] {
        std::vector<uint8_t> frame(100, 0);
        frame[0] = 255;
        frame[1] = 255;
        Check(GreyFrameIsBlack(frame.data(), frame.size()),
              "98 dark pixels in 100 are a black frame");
        frame[2] = 26;
        Check(!GreyFrameIsBlack(frame.data(), frame.size()),
              "97 are not; 26 is over a tenth of full scale");
        frame[2] = 25;
        Check(GreyFrameIsBlack(frame.data(), frame.size()),
              "25 is still black");
    });

    // Out of lot -- the author, 31 October 2026: a cut inside one rush that the
    // film shows as it is.
    Test("a cut inside one rush, shown as it is, is a jump cut", [] {
        const Document document = Interview();
        TimelineCheckReport report = Run(document);
        Check(report.jump_cuts.size() == 1 &&
                  report.jump_cuts[0].frame == 100 &&
                  report.jump_cuts[0].timeline_in == RationalTime(100, 25) &&
                  report.jump_cuts[0].track_id == kPictureTrack &&
                  report.jump_cuts[0].outgoing_clip_id == kFirst &&
                  report.jump_cuts[0].incoming_clip_id == kSecond &&
                  report.jump_cuts[0].source_id == kSource,
              "the two shots of one rush on V1 make one jump, at frame 100");
        Check(StatusOf(report, "jump_cuts") == TimelineCheckStatus::NotRun &&
                  report.Passed(),
              "listed by default, judged by none");
        const std::string json = SerializeTimelineCheck(report);
        Check(json.find("\"jump_cuts\":[{\"frame\":100,\"timeline_in\":"
                        "{\"value\":100,\"rate\":25},\"track_id\":\"" +
                        kPictureTrack + "\",\"outgoing_clip_id\":\"" + kFirst +
                        "\",\"incoming_clip_id\":\"" + kSecond +
                        "\",\"source_id\":\"" + kSource + "\"}]") !=
                  std::string::npos,
              "the report lists it whatever the policy: " + json);

        report = Run(document, Parse("{\"jump_cuts\":\"forbidden\"}"));
        Check(StatusOf(report, "jump_cuts") == TimelineCheckStatus::Failed &&
                  !report.Passed() &&
                  Find(report, "jump_cuts")->reason ==
                      "1 cut(s) inside one rush show the same framing on "
                      "both sides, with nothing over them",
              "the interview's policy fails it");
    });

    Test("a cutaway, a reframe or a through edit breaks no rule", [] {
        const Ulid other = "01KA000000000000000000000H";
        const Ulid cutawayTrack = "01KA000000000000000000000J";
        const Ulid cutaway = "01KA000000000000000000000K";
        const TimelinePolicy forbidden = Parse("{\"jump_cuts\":\"forbidden\"}");
        // A shot of another rush on V2, from frame `from` to `until`.
        const auto Covered = [&](int64_t from, int64_t until) {
            Document document = Interview();
            DocumentSource source = document.sources[0];
            source.id = other;
            source.path = "broll.mov";
            document.sources.push_back(source);
            DocumentTrack track;
            track.id = cutawayTrack;
            track.kind = "video";
            track.index = 2;
            DocumentClip clip;
            clip.id = cutaway;
            clip.source_id = other;
            clip.source_in = RationalTime(0, 25);
            clip.timeline_in = RationalTime(from, 25);
            clip.duration = RationalTime(until - from, 25);
            track.clips = {clip};
            document.sequence.tracks.push_back(track);
            return document;
        };
        Check(Run(Covered(90, 110), forbidden).jump_cuts.empty(),
              "a cutaway over the cut hides it");
        Check(Run(Covered(80, 100), forbidden).jump_cuts.empty() &&
                  Run(Covered(100, 120), forbidden).jump_cuts.empty(),
              "so does one that ends or starts at it: one side is the "
              "cutaway");
        Check(Run(Covered(20, 60), forbidden).jump_cuts.size() == 1,
              "a cutaway elsewhere does not");

        Document reframed = Interview();
        reframed.sequence.tracks[0].clips[1].framing.scale = {4, 3};
        Check(Run(reframed, forbidden).jump_cuts.empty() &&
                  StatusOf(Run(reframed, forbidden), "jump_cuts") ==
                      TimelineCheckStatus::Passed,
              "a punch-in on the second shot breaks the jump");
        Document spelled = Interview();
        spelled.sequence.tracks[0].clips[1].framing.scale = {2, 2};
        Check(Run(spelled, forbidden).jump_cuts.size() == 1,
              "a framing spelled differently but equal is the same framing");

        Document through = Interview();
        through.sequence.tracks[0].clips[1].source_in = RationalTime(200, 25);
        through.sequence.tracks[1].clips[1].source_in = RationalTime(200, 25);
        Check(Run(through, forbidden).jump_cuts.empty(),
              "a through edit shows nothing");

        Document hidden = Interview();
        hidden.sequence.tracks[0].visible = false;
        Check(Run(hidden, forbidden).jump_cuts.empty(),
              "a hidden track shows no jump");
    });

    Test("a title over the cut does not hide the jump", [] {
        Document document = Branded();
        for (DocumentTrack& track : document.sequence.tracks) {
            if (track.id == kTitleTrack) {
                track.clips[0].duration = RationalTime(200, 25);
            }
        }
        Check(Run(document).jump_cuts.size() == 1,
              "a motion clip is drawn over the cut, not instead of it");
    });

    Test("the jump-cut rule reads as written", [] {
        const TimelinePolicy forbidden = Parse("{\"jump_cuts\":\"forbidden\"}");
        Check(forbidden.jump_cuts == JumpCutRule::Forbidden &&
                  forbidden.name == "custom",
              "forbidding them changes the default");
        Check(Parse("{\"jump_cuts\":\"allowed\"}").name == "default",
              "allowing them is the default");
        Check(Refusal("{\"jump_cuts\":\"some\"}") ==
                  "'policy.jump_cuts' must be \"forbidden\" or \"allowed\"",
              "an unknown rule is refused");
    });

    Test("an empty timeline has no end to judge", [] {
        Document document = Interview();
        document.sequence.tracks[0].clips.clear();
        document.sequence.tracks[1].clips.clear();
        const TimelineCheckReport report = Run(document);
        Check(
            StatusOf(report, "final_black") == TimelineCheckStatus::NotRun &&
                Find(report, "final_black")->reason == "the timeline is empty",
            "not run, and why");
    });

    // PA2, PA11 -- docs/roadmap/PARITE-2026-11.md.
    Test("PA2: the loudness and the shape are judged only when asked", [] {
        const TimelineCheckReport plain = Run(Interview());
        Check(Find(plain, "loudness") == nullptr &&
                  Find(plain, "format") == nullptr && plain.checks.size() == 7,
              "a policy that names neither keeps its seven checks");
        const std::string json = SerializeTimelineCheck(plain);
        Check(json.find("loudness") == std::string::npos &&
                  json.find("aspect_ratio") == std::string::npos,
              "and its report says nothing of them");
    });

    Test("PA2: the delivered loudness against its target", [] {
        const TimelinePolicy policy = Parse(R"({"loudness_target":-14})");
        Check(policy.loudness_target_lufs &&
                  *policy.loudness_target_lufs == -14 &&
                  policy.true_peak_max_dbtp == -1 && policy.name == "custom",
              "a target reads as whole LUFS, its ceiling -1 dBTP");
        const TimelineCheckReport unmeasured = Run(Interview(), policy);
        Check(StatusOf(unmeasured, "loudness") == TimelineCheckStatus::NotRun &&
                  Find(unmeasured, "loudness")->reason ==
                      "no delivered file was measured",
              "without a file the loudness is not run, never passed");

        TimelineFileMeasures file;
        file.final_frames_black = {false};
        file.loudness_measured = true;
        file.integrated_lufs = -14.6;
        file.true_peak_dbfs = -3.4;
        Check(StatusOf(Run(Interview(), policy, &file), "loudness") ==
                  TimelineCheckStatus::Passed,
              "within one loudness unit and under the ceiling passes");
        const std::string json =
            SerializeTimelineCheck(Run(Interview(), policy, &file));
        Check(json.find(R"("integrated_lufs":-14.6)") != std::string::npos &&
                  json.find(R"("true_peak_dbtp":-3.4)") != std::string::npos &&
                  json.find(R"("loudness_target":-14,"true_peak_max":-1)") !=
                      std::string::npos,
              "the report carries the measure and the policy: " + json);

        file.integrated_lufs = -15.3;
        const TimelineCheckReport quiet = Run(Interview(), policy, &file);
        Check(StatusOf(quiet, "loudness") == TimelineCheckStatus::Failed &&
                  Find(quiet, "loudness")->reason ==
                      "the delivered file is quieter than loudness_target",
              "more than one unit under fails, and says which way");
        file.integrated_lufs = -12.5;
        Check(Find(Run(Interview(), policy, &file), "loudness")->reason ==
                  "the delivered file is louder than loudness_target",
              "more than one unit over fails too");
        file.integrated_lufs = -14.0;
        file.true_peak_dbfs = -0.2;
        Check(Find(Run(Interview(), policy, &file), "loudness")->reason ==
                  "the delivered file peaks above true_peak_max",
              "a peak over the ceiling fails a file at the right loudness");
        file.true_peak_dbfs = -0.2;
        Check(StatusOf(Run(Interview(),
                           Parse(R"({"loudness_target":-14,)"
                                 R"("true_peak_max":0})"),
                           &file),
                       "loudness") == TimelineCheckStatus::Passed,
              "unless the policy lets it peak there");
        file.integrated_lufs.reset();
        Check(Find(Run(Interview(), policy, &file), "loudness")->reason ==
                  "the delivered file is silent",
              "a silent file fails a loudness target");
        file.loudness_measured = false;
        file.loudness_error = "no audio stream";
        const TimelineCheckReport unread = Run(Interview(), policy, &file);
        Check(StatusOf(unread, "loudness") == TimelineCheckStatus::NotRun &&
                  Find(unread, "loudness")->reason == "no audio stream",
              "a file whose sound could not be read is not run, with why");

        Check(Refusal(R"({"loudness_target":-4})") ==
                      "'policy.loudness_target' must be a whole number of "
                      "LUFS from -40 to -5" &&
                  Refusal(R"({"loudness_target":"-14"})") ==
                      "'policy.loudness_target' must be a whole number of "
                      "LUFS from -40 to -5",
              "a target out of range or not a number is refused");
        Check(Refusal(R"({"true_peak_max":-1})") ==
                  "'policy.true_peak_max' is judged with the loudness: set "
                  "'policy.loudness_target' too",
              "a ceiling alone is refused: nothing would judge it");
    });

    Test("PA11: the sequence's shape against the policy's", [] {
        // The interview fixture is 1920 by 1080.
        Document document = Interview();
        document.sequence.width = 1920;
        document.sequence.height = 1080;
        Check(StatusOf(Run(document, Parse(R"({"aspect_ratio":"16:9"})")),
                       "format") == TimelineCheckStatus::Passed,
              "a 1920 by 1080 sequence is 16:9");
        const TimelineCheckReport vertical =
            Run(document, Parse(R"({"aspect_ratio":"9:16"})"));
        Check(
            StatusOf(vertical, "format") == TimelineCheckStatus::Failed &&
                Find(vertical, "format")->reason == "the sequence is not 9:16",
            "and is refused as 9:16, by name");
        document.sequence.width = 1080;
        document.sequence.height = 1350;
        Check(StatusOf(Run(document, Parse(R"({"aspect_ratio":"4:5"})")),
                       "format") == TimelineCheckStatus::Passed,
              "1080 by 1350 is 4:5");
        Check(Refusal(R"({"aspect_ratio":"vertical"})") ==
                  "'policy.aspect_ratio' must be two whole numbers, as "
                  "\"9:16\"",
              "a shape that is not a ratio is refused");
    });

    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    return 0;
}
