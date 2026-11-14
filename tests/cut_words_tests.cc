#include "CutWords.h"

#include "Json.h"
#include "Operations.h"

#include <iostream>
#include <map>
#include <string>
#include <vector>

// IA9 -- ROADMAP.md. Le cœur est pur : deux documents, des mots en mémoire.
// Ce qui est épinglé ici, c'est ce que le ticket promet -- un mot entier
// retiré, un mot entamé et un mot visé sont nommés et classés, une coupe dans
// le silence ne nomme rien -- et que la matière retirée se lit bien sur les
// pistes son, quelle que soit l'opération qui l'a retirée.

namespace {

int failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

const char kSource[] = "01K40000000000000000000001";
const char kOther[] = "01K40000000000000000000002";
const char kPicture[] = "01K40000000000000000000010";
const char kSound[] = "01K40000000000000000000011";
const char kVideoTrack[] = "01K40000000000000000000020";
const char kAudioTrack[] = "01K40000000000000000000021";
const char kSecondAudioTrack[] = "01K40000000000000000000022";
const char kLink[] = "01K40000000000000000000030";

RationalTime F(int64_t frame) { return {frame, 25}; }

DocumentClip Clip(const std::string& id, int64_t sourceIn, int64_t frames,
                  int64_t timelineIn, const std::string& source = kSource) {
    DocumentClip clip;
    clip.id = id;
    clip.source_id = source;
    clip.source_in = F(sourceIn);
    clip.duration = F(frames);
    clip.timeline_in = F(timelineIn);
    return clip;
}

// Une paire image/son de 100 images sur la source.
Document Pair() {
    Document document;
    document.sources = {{kSource, "rush.MP4", {25, 1}, {1000, 25}},
                        {kOther, "autre.MP4", {25, 1}, {1000, 25}}};
    DocumentClip picture = Clip(kPicture, 0, 100, 0);
    DocumentClip sound = Clip(kSound, 0, 100, 0);
    picture.link_group_id = kLink;
    sound.link_group_id = kLink;
    document.sequence.tracks = {{kVideoTrack, "video", 0, {picture}},
                                {kAudioTrack, "audio", 1, {sound}}};
    return document;
}

// Mots alignés : « Alors » [10,20), « à » [30,35), « demain » [36,45),
// « matin » [50,60).
Transcript Words(const std::string& timing = "nemo") {
    Transcript transcript;
    transcript.media_id = kSource;
    transcript.source_rate = {25, 1};
    transcript.word_timing = timing;
    transcript.words = {{"Alors", F(10), F(20)},
                        {"à", F(30), F(35)},
                        {"demain", F(36), F(45)},
                        {"matin", F(50), F(60)}};
    return transcript;
}

std::string Texts(const std::vector<CutWord>& words) {
    std::string text;
    for (const CutWord& word : words) {
        if (!text.empty()) text += ' ';
        text += word.text + (word.whole ? "" : "~");
    }
    return text;
}

}  // namespace

int main() {
    // --- Un mot entier, un mot entamé, un mot visé -----------------------
    // Le son perd [30,40) : « à » entier, le début de « demain ». « à » était
    // demandé ; « demain » est emporté.
    {
        Document before = Pair();
        Document after = before;
        after.sequence.tracks[1].clips = {
            Clip(kSound, 0, 30, 0),
            Clip("01K40000000000000000000012", 40, 60, 30)};
        const std::vector<RemovedSourceRange> removed =
            RemovedAudioMaterial(before, after);
        Check(removed.size() == 1 && removed[0].source_id == kSource &&
                  removed[0].start == F(30) && removed[0].end == F(40),
              "la matière retirée est la plage que le son ne joue plus");
        const CutWordsReport report =
            CheckCutWords(removed, {{kSource, Words()}}, {{kSource, {{1, 1}}}});
        Check(report.checked && report.sources.size() == 1 &&
                  report.sources[0].word_timing == "nemo",
              "une source alignée est vérifiée");
        if (report.sources.size() == 1) {
            Check(Texts(report.sources[0].targeted) == "à",
                  "le mot demandé est classé parmi les visés : " +
                      Texts(report.sources[0].targeted));
            Check(Texts(report.sources[0].carried) == "demain~",
                  "le mot entamé est emporté et marqué entamé : " +
                      Texts(report.sources[0].carried));
        }
        const CutWordsReport untargeted =
            CheckCutWords(removed, {{kSource, Words()}}, {});
        Check(untargeted.sources.size() == 1 &&
                  Texts(untargeted.sources[0].carried) == "à demain~" &&
                  untargeted.sources[0].targeted.empty(),
              "sans mot demandé, tout mot touché est emporté");
    }

    // AGENT-2026-09 A45 -- un mot que l'aligneur a fait d'une image, à
    // moins de trois images d'une coupe, est signalé à risque : sa voix
    // déborde de son image (« pour », emporté par le retrait de « justement »
    // sur un vrai montage, sans que le rapport le dise).
    {
        Document before = Pair();
        Document after = before;
        after.sequence.tracks[1].clips = {
            Clip(kSound, 0, 30, 0),
            Clip("01K40000000000000000000012", 40, 60, 30)};
        Transcript words = Words();
        words.words = {{"Alors", F(10), F(20)},
                       {"pour", F(28), F(29)},
                       {"à", F(30), F(35)},
                       {"demain", F(36), F(45)},
                       {"matin", F(50), F(60)}};
        const CutWordsReport report = CheckCutWords(
            RemovedAudioMaterial(before, after), {{kSource, words}}, {});
        Check(
            report.sources.size() == 1 &&
                Texts(report.sources[0].at_risk) == "pour~" &&
                Texts(report.sources[0].carried) == "à demain~",
            "un mot d'une image contre la coupe est à risque, pas emporté : " +
                (report.sources.empty() ? std::string("(rien)")
                                        : Texts(report.sources[0].at_risk)));
        Check(SerializeCutWordsReport(report).find("\"at_risk\"") !=
                  std::string::npos,
              "le rapport sérialisé porte les mots à risque");
    }

    // --- Une coupe dans le silence ne nomme aucun mot ---------------------
    {
        Document before = Pair();
        Document after = before;
        after.sequence.tracks[1].clips = {
            Clip(kSound, 0, 22, 0),
            Clip("01K40000000000000000000012", 28, 72, 22)};
        const CutWordsReport report = CheckCutWords(
            RemovedAudioMaterial(before, after), {{kSource, Words()}}, {});
        Check(report.checked && report.sources.size() == 1 &&
                  report.sources[0].targeted.empty() &&
                  report.sources[0].carried.empty(),
              "une coupe entre deux mots ne nomme rien, et reste vérifiée");
    }

    // --- Sans alignement, la réponse le dit ------------------------------
    {
        Document before = Pair();
        Document after = before;
        after.sequence.tracks[1].clips = {Clip(kSound, 0, 30, 0)};
        const std::vector<RemovedSourceRange> removed =
            RemovedAudioMaterial(before, after);
        const CutWordsReport none = CheckCutWords(removed, {}, {});
        Check(!none.checked && none.sources.size() == 1 &&
                  !none.sources[0].checked &&
                  none.sources[0].unchecked_reason.find("no transcript") !=
                      std::string::npos,
              "une source sans transcript est déclarée non vérifiée");
        const CutWordsReport whisper =
            CheckCutWords(removed, {{kSource, Words("whisper")}}, {});
        Check(!whisper.checked && whisper.sources.size() == 1 &&
                  whisper.sources[0].carried.empty() &&
                  whisper.sources[0].unchecked_reason.find("Whisper") !=
                      std::string::npos,
              "des bornes Whisper ne nomment aucun mot, et la raison est "
              "donnée");
    }

    // --- Ce qui ne retire rien du son n'est pas une perte ------------------
    {
        Document before = Pair();
        // L'image seule est rognée : aucun son ne part.
        Document picture = before;
        picture.sequence.tracks[0].clips = {Clip(kPicture, 30, 70, 30)};
        Check(RemovedAudioMaterial(before, picture).empty(),
              "rogner l'image ne retire aucun mot entendu");

        // Le son passe sur une autre piste, et plus loin dans le temps.
        Document moved = before;
        moved.sequence.tracks[1].clips.clear();
        DocumentClip sound = Clip(kSound, 0, 100, 200);
        moved.sequence.tracks.push_back(
            {kSecondAudioTrack, "audio", 2, {sound}});
        Check(RemovedAudioMaterial(before, moved).empty(),
              "déplacer le son, même d'une piste à l'autre, ne retire rien");

        // Le même rush est posé deux fois ; une seule copie perd [30,40).
        Document twice = before;
        twice.sequence.tracks.push_back(
            {kSecondAudioTrack,
             "audio",
             2,
             {Clip("01K40000000000000000000013", 0, 100, 0)}});
        Document trimmedCopy = twice;
        trimmedCopy.sequence.tracks[2].clips = {
            Clip("01K40000000000000000000013", 0, 30, 0),
            Clip("01K40000000000000000000014", 40, 60, 30)};
        const std::vector<RemovedSourceRange> copy =
            RemovedAudioMaterial(twice, trimmedCopy);
        Check(
            copy.size() == 1 && copy[0].start == F(30) && copy[0].end == F(40),
            "une copie entamée d'un rush posé deux fois compte comme une "
            "perte");

        // Deux sources, chacune perd sa part : deux entrées, dans l'ordre.
        Document two = before;
        two.sequence.tracks[1].clips.push_back(
            Clip("01K40000000000000000000015", 0, 50, 100, kOther));
        Document bothCut = two;
        bothCut.sequence.tracks[1].clips = {
            Clip(kSound, 0, 90, 0),
            Clip("01K40000000000000000000015", 5, 45, 90, kOther)};
        const std::vector<RemovedSourceRange> both =
            RemovedAudioMaterial(two, bothCut);
        Check(both.size() == 2 && both[0].source_id == kSource &&
                  both[0].start == F(90) && both[0].end == F(100) &&
                  both[1].source_id == kOther && both[1].start == F(0) &&
                  both[1].end == F(5),
              "deux sources touchées donnent deux plages, chacune sur sa "
              "source");
    }

    // --- De vraies opérations ----------------------------------------------
    // La lecture par documents doit voir ce que les opérations du moteur
    // retirent, paire liée comprise.
    {
        Document before = Pair();
        Document after = before;
        Operation trim = RippleTrimOperation{
            kSound, TrimEdge::Head, F(12), {kPicture}, {}, {}};
        Operation inverse = RemoveClipOperation{};
        EditError error = EditError::None;
        std::string message;
        Check(ApplyOperation(after, trim, inverse, error, message),
              "le ripple trim s'applique : " + message);
        const CutWordsReport report = CheckCutWords(
            RemovedAudioMaterial(before, after), {{kSource, Words()}}, {});
        Check(report.sources.size() == 1 &&
                  report.sources[0].removed.size() == 1 &&
                  report.sources[0].removed[0].end == F(12) &&
                  Texts(report.sources[0].carried) == "Alors~",
              "rogner la tête de 12 images entame « Alors » : " +
                  (report.sources.empty() ? std::string()
                                          : Texts(report.sources[0].carried)));

        Document words = Pair();
        Document cut = words;
        RemoveWordsOperation removal;
        removal.clip_id = kPicture;
        removal.ranges = {{F(30), F(35)}, {F(48), F(62)}};
        removal.linked_clip_ids = {kSound};
        Operation removeWords = removal;
        Check(ApplyOperation(cut, removeWords, inverse, error, message),
              "RemoveWords s'applique : " + message);
        const CutWordsReport removed =
            CheckCutWords(RemovedAudioMaterial(words, cut),
                          {{kSource, Words()}}, {{kSource, {{1, 1}}}});
        Check(removed.sources.size() == 1 &&
                  removed.sources[0].removed.size() == 2 &&
                  Texts(removed.sources[0].targeted) == "à" &&
                  Texts(removed.sources[0].carried) == "matin",
              "le son lié perd les deux plages ; « matin » part avec la "
              "seconde");
    }

    // --- Sérialisation ------------------------------------------------------
    {
        Document before = Pair();
        Document after = before;
        after.sequence.tracks[1].clips = {
            Clip(kSound, 0, 30, 0),
            Clip("01K40000000000000000000012", 40, 60, 30)};
        const std::string json = SerializeCutWordsReport(
            CheckCutWords(RemovedAudioMaterial(before, after),
                          {{kSource, Words()}}, {{kSource, {{1, 1}}}}));
        mcp_json::Value parsed;
        std::string parseError;
        Check(mcp_json::Value::Parse(json, parsed, parseError) &&
                  parsed.IsObject(),
              "le rapport est du JSON valide : " + parseError);
        const mcp_json::Value* sources = parsed.Find("sources");
        const mcp_json::Value* source = sources != nullptr &&
                                                sources->IsArray() &&
                                                sources->AsArray().size() == 1
                                            ? &sources->AsArray()[0]
                                            : nullptr;
        const mcp_json::Value* carried =
            source != nullptr ? source->Find("carried") : nullptr;
        const mcp_json::Value* word = carried != nullptr &&
                                              carried->IsArray() &&
                                              carried->AsArray().size() == 1
                                          ? &carried->AsArray()[0]
                                          : nullptr;
        int64_t first = -1;
        int64_t end = -1;
        Check(word != nullptr && word->Find("first_frame")->AsInt64(first) &&
                  word->Find("end_frame")->AsInt64(end) && first == 36 &&
                  end == 45 && !word->Find("whole")->AsBool() &&
                  word->Find("text")->AsString() == "demain",
              "un mot emporté se lit en images de la source : " + json);
    }

    if (failures != 0) {
        std::cerr << failures << " assertion(s) en échec\n";
        return 1;
    }
    std::cout << "Tous les tests des mots touchés par une coupe passent\n";
    return 0;
}
