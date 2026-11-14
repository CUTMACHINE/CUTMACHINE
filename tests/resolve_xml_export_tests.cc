// R9 -- docs/roadmap/REEL-2026-09.md. Le renvoi par fichier : vitesses et
// fondus enchaînés, dans les unités du xmeml (images de séquence après
// vitesse). Rien ici ne touche Resolve : l'écrivain est du C++ portable.

#include "ClipSpeed.h"
#include "Document.h"
#include "ProjectStorage.h"
#include "ResolveXmlExport.h"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

namespace {

int failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

LibraryMedia Media(const std::string& id, const std::string& name,
                   int32_t rateNum, int64_t frames) {
    LibraryMedia media;
    media.id = id;
    media.filename = name;
    media.path = "/rushes/" + name;
    media.codec = "h264";
    media.width = 2160;
    media.height = 3840;
    media.orientation = "portrait";
    media.rate = {rateNum, 1};
    media.duration = {frames, rateNum};
    return media;
}

DocumentClip Clip(const std::string& id, const std::string& sourceId,
                  RationalTime timelineIn, RationalTime sourceIn,
                  RationalTime duration, EffectParamValue speed = {1, 1}) {
    DocumentClip clip;
    clip.id = id;
    clip.source_id = sourceId;
    clip.timeline_in = timelineIn;
    clip.source_in = sourceIn;
    clip.duration = duration;
    clip.speed = speed;
    return clip;
}

Document Montage() {
    Document document;
    document.sequence.name = "MONTAGE";
    document.sequence.frame_rate = {25, 1};
    document.sequence.width = 1080;
    document.sequence.height = 1920;
    // A tourné à 50 i/s, B à 25 i/s : les deux cas que le critère 1 demande.
    document.library = {Media("01K30000000000000000000001", "A.MP4", 50, 552),
                        Media("01K30000000000000000000002", "B.MP4", 25, 360)};
    DocumentTrack base;
    base.id = "01K30000000000000000000010";
    base.kind = "video";
    base.index = 0;
    base.clips = {
        // 1/2 sur un rush à 50 i/s : une image source par image de séquence.
        Clip("01K30000000000000000000020", "01K30000000000000000000001",
             {0, 25}, {100, 50}, {50, 25}, {1, 2}),
        // 1/2 sur un rush à 25 i/s : chaque image source vue deux fois.
        Clip("01K30000000000000000000021", "01K30000000000000000000002",
             {50, 25}, {50, 25}, {40, 25}, {1, 2}),
        // Vitesse 1, entrée sur une image source paire : convertible.
        Clip("01K30000000000000000000022", "01K30000000000000000000001",
             {100, 25}, {200, 50}, {60, 25}),
    };
    DocumentTrack overlay;
    overlay.id = "01K30000000000000000000011";
    overlay.kind = "video";
    overlay.index = 1;
    overlay.clips = {Clip("01K30000000000000000000023",
                          "01K30000000000000000000001", {30, 25}, {300, 50},
                          {20, 25}, {2, 1})};
    DocumentTrack sound;
    sound.id = "01K30000000000000000000012";
    sound.kind = "audio";
    sound.index = 0;
    document.sequence.tracks = {base, overlay, sound};

    DocumentTransition dissolve;
    dissolve.id = "01K30000000000000000000030";
    dissolve.track_id = base.id;
    dissolve.left_clip_id = "01K30000000000000000000020";
    dissolve.right_clip_id = "01K30000000000000000000021";
    dissolve.type = "cross_dissolve";
    dissolve.duration = {12, 25};
    dissolve.alignment = TransitionAlignment::Center;
    document.sequence.transitions = {dissolve};
    return document;
}

// R2 -- docs/roadmap/REEL-2026-09.md. Two keys on one clip, the smallest
// animation the document accepts.
std::vector<ClipParameterAnimation> ZoomTrack() {
    ClipKeyframe first;
    first.id = "01K30000000000000000000K01";
    first.time = {0, 25};
    first.value = EffectParamValue{1, 1};
    ClipKeyframe last;
    last.id = "01K30000000000000000000K02";
    last.time = {12, 25};
    last.value = EffectParamValue{23, 20};
    ClipParameterAnimation track;
    track.parameter = ClipAnimatedParameter::FramingScale;
    track.keyframes = {first, last};
    return {track};
}

// FI6 -- docs/roadmap/FIABILITE-2026-10.md. Real files and a real package:
// all entry points must refuse a destination that already names anything.
void TestPublication() {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / (GenerateUlid() + "-xml");
    fs::create_directory(root);
    Document doc = Montage();
    doc.sequence.id = GenerateUlid();
    doc.sequence.transitions.clear();
    doc.sequence.tracks.resize(1);
    doc.sequence.tracks[0].clips.resize(1);
    doc.library.resize(1);
    doc.library[0].path = (root / "A.MP4").string();
    const auto& media = doc.library[0];
    doc.sources = {{media.id, media.path, media.rate, media.duration}};
    const std::string sourceBytes = "temporary source sentinel";
    std::ofstream(media.path) << sourceBytes;
    const auto read = [](const fs::path& path) {
        std::ifstream input(path, std::ios::binary);
        std::ostringstream bytes;
        bytes << input.rdbuf();
        return bytes.str();
    };
    std::string projectPath, error, report, errorName;
    Check(CreatePortableProject((root / "project").string(),
                                Project::FromDocument(doc), projectPath, error),
          "the temporary project saves: " + error);
    if (projectPath.empty()) {
        fs::remove_all(root);
        return;
    }
    const auto originalProject = read(projectPath);
    Check(ExportResolveXmlCommand(projectPath, projectPath, report) != 0 &&
              report.find("OutputExists") != std::string::npos &&
              read(projectPath) == originalProject,
          "the CLI refuses to overwrite its project: " + report);
    const auto write = [&](const fs::path& path) {
        return BuildAndWriteResolveXml(doc, root.string(), path.string(),
                                       report, errorName, error);
    };
    Check(!write(media.path) && errorName == "OutputExists" &&
              read(media.path) == sourceBytes,
          "the writer preserves the source media");
    const auto linked = root / "linked.xml";
    fs::create_symlink(projectPath, linked);
    Check(!write(linked) && errorName == "OutputExists" &&
              fs::is_symlink(linked) && read(projectPath) == originalProject,
          "a symlink neither replaces nor truncates the project");
    const auto dangling = root / "dangling.xml";
    fs::create_symlink(root / "missing", dangling);
    Check(!write(dangling) && errorName == "OutputExists" &&
              fs::is_symlink(dangling) && !fs::exists(root / "missing"),
          "a dangling symlink is preserved too");
    const auto hard = root / "hard.xml";
    fs::create_hard_link(media.path, hard);
    Check(!write(hard) && errorName == "OutputExists" &&
              read(media.path) == sourceBytes,
          "a hard link cannot be used to truncate the source");

    const auto output = root / "delivery.xml";
    Check(write(output), "a new delivery succeeds: " + error);
    const auto xml = read(output);
    Check(
        xml.rfind("<?xml", 0) == 0 && xml.find("</xmeml>") != std::string::npos,
        "the published XML is complete");
    Check(!write(output) && errorName == "OutputExists" && read(output) == xml,
          "a previous delivery remains unchanged");
    Check(!write(root / "missing-parent" / "delivery.xml") &&
              errorName == "IoError",
          "an inaccessible destination is an I/O failure");

    const auto raced = root / "raced.xml";
    std::array<bool, 2> ok{};
    const auto compete = [&](size_t index) {
        std::string result, name, message;
        ok[index] = BuildAndWriteResolveXml(doc, root.string(), raced.string(),
                                            result, name, message);
    };
    std::thread first(compete, 0);
    std::thread second(compete, 1);
    first.join();
    second.join();
    Check(ok[0] != ok[1] && read(raced) == xml,
          "two writers publish exactly one complete delivery");
    for (const auto& entry : fs::directory_iterator(root)) {
        Check(entry.path().filename().string().find(".cutmachine-xml-") != 0,
              "no staging file remains after publication or refusal");
    }
#ifdef __APPLE__
    // FI6 -- an exFAT volume, as an external SSD is sold, has neither an
    // exclusive rename nor hard links: the delivery is published there too,
    // and a second one is still refused. A real volume, from a disk image.
    const auto image = root / "exfat.dmg";
    const auto volume = root / "exfat";
    const std::string quotedImage = "'" + image.string() + "'";
    const std::string quotedVolume = "'" + volume.string() + "'";
    if (std::system(("hdiutil create -quiet -size 8m -fs ExFAT -volname "
                     "CMXML " +
                     quotedImage)
                        .c_str()) == 0 &&
        std::system(("hdiutil attach -quiet -nobrowse -mountpoint " +
                     quotedVolume + " " + quotedImage)
                        .c_str()) == 0) {
        const auto onExfat = volume / "delivery.xml";
        Check(write(onExfat) && read(onExfat) == xml,
              "a delivery is published on an exFAT volume: " + error);
        Check(!write(onExfat) && errorName == "OutputExists" &&
                  read(onExfat) == xml,
              "and a second one is refused there too");
        for (const auto& entry : fs::directory_iterator(volume)) {
            Check(
                entry.path().filename().string().find(".cutmachine-xml-") != 0,
                "no staging file remains on the exFAT volume");
        }
        std::system(("hdiutil detach -quiet -force " + quotedVolume).c_str());
    } else {
        std::cout << "exFAT publication not checked: no disk image could be "
                     "mounted here\n";
    }
#endif
    fs::remove_all(root);
}

}  // namespace

int main() {
    TestPublication();
    // --- Le pourcentage du filtre Time Remap ------------------------------
    // Le format n'a pas de rationnel : la fraction devient un pourcentage,
    // développé par division entière. Aucun double ne touche une vitesse.
    Check(FormatSpeedPercent({1, 1}) == "100", "1/1 vaut 100 %");
    Check(FormatSpeedPercent({1, 2}) == "50", "1/2 vaut 50 %");
    Check(FormatSpeedPercent({2, 1}) == "200", "2 vaut 200 %");
    Check(FormatSpeedPercent({2, 5}) == "40", "2/5 vaut 40 %");
    Check(FormatSpeedPercent({1, 8}) == "12.5",
          "une fraction qui termine s'écrit exactement : " +
              FormatSpeedPercent({1, 8}));
    Check(FormatSpeedPercent({1, 3}) == "33.333333333333",
          "une fraction qui ne termine pas est tronquée, pas arrondie au "
          "hasard : " +
              FormatSpeedPercent({1, 3}));

    // --- Construction -----------------------------------------------------
    ResolveXmlTimeline timeline;
    std::string error;
    Check(BuildResolveXmlTimeline(Montage(), timeline, error),
          "un montage à vitesses et fondu s'écrit : " + error);
    Check(timeline.name == "MONTAGE" && timeline.width == 1080 &&
              timeline.height == 1920 && timeline.frame_rate.num == 25,
          "le format de séquence est transmis");
    Check(timeline.clips.size() == 4, "les quatre plans vidéo sont écrits");
    Check(timeline.media.size() == 2,
          "un rush lu par deux plans n'est décrit qu'une fois");
    Check(timeline.audio_tracks_not_sent == 1,
          "la piste son laissée derrière est comptée, pas ignorée en "
          "silence");
    Check(timeline.duration == 160, "la séquence va jusqu'au dernier plan");

    // La conversion que le ticket ajoute : la plage source en images de
    // séquence APRÈS vitesse. Écrite en images source, elle désignerait une
    // autre plage du rush.
    Check(
        timeline.clips[0].media_in == 100 && timeline.clips[0].media_out == 150,
        "1/2 sur du 50 i/s : l'image source 100 s'écrit 100");
    Check(
        timeline.clips[0].record_in == 0 && timeline.clips[0].record_out == 50,
        "la position et la durée restent en images de séquence");
    Check(
        timeline.clips[1].media_in == 100 && timeline.clips[1].media_out == 140,
        "1/2 sur du 25 i/s : l'image source 50 s'écrit 100");
    Check(
        timeline.clips[2].media_in == 100 && timeline.clips[2].media_out == 160,
        "vitesse 1 sur du 50 i/s : l'image source 200 s'écrit 100");
    Check(timeline.clips[3].video_layer == 1 &&
              timeline.clips[3].media_in == 75 &&
              timeline.clips[3].record_in == 30,
          "le plan de recouvrement nomme sa couche, et la vitesse 2 divise "
          "son entrée");

    Check(timeline.transitions.size() == 1, "le fondu enchaîné est écrit");
    Check(timeline.transitions[0].start == 44 &&
              timeline.transitions[0].end == 56 &&
              timeline.transitions[0].alignment == "center",
          "un fondu centré de 12 images encadre la coupe à 50");
    Check(timeline.transitions[0].video_layer == 0,
          "le fondu reste sur la piste de ses deux plans");

    // --- R5 : ouverture et fermeture au noir ------------------------------
    // Mesuré le 19 septembre sur un XML fabriqué à la main : « start-black »
    // et « end-black » passent l'import. Ce que le document y met depuis R5
    // n'a PAS été remis devant Resolve.
    {
        Document edges = Montage();
        DocumentTransition opening;
        opening.id = "01K30000000000000000000031";
        opening.track_id = edges.sequence.tracks[0].id;
        opening.right_clip_id = edges.sequence.tracks[0].clips[0].id;
        opening.type = "dip_to_color";
        opening.duration = {40, 25};
        opening.alignment = TransitionAlignment::StartAtCut;
        DocumentTransition closing;
        closing.id = "01K30000000000000000000032";
        closing.track_id = edges.sequence.tracks[0].id;
        closing.left_clip_id = edges.sequence.tracks[0].clips[2].id;
        closing.type = "dip_to_color";
        closing.duration = {50, 25};
        closing.alignment = TransitionAlignment::EndAtCut;
        edges.sequence.transitions.push_back(opening);
        edges.sequence.transitions.push_back(closing);
        // Montage() ne porte que sa bibliothèque, pas ses sources : la
        // validité de ces deux transitions est éprouvée dans
        // tests/transition_render_tests.cc, sur un document complet.
        std::string edgeError;
        ResolveXmlTimeline edgeTimeline;
        Check(BuildResolveXmlTimeline(edges, edgeTimeline, edgeError),
              "il s'écrit : " + edgeError);
        Check(edgeTimeline.transitions.size() == 3,
              "les trois transitions sortent");
        Check(edgeTimeline.transitions[0].start == 0 &&
                  edgeTimeline.transitions[0].end == 40 &&
                  edgeTimeline.transitions[0].alignment == "start-black",
              "l'ouverture : 0 à 40, start-black");
        Check(edgeTimeline.transitions[2].start == 110 &&
                  edgeTimeline.transitions[2].end == 160 &&
                  edgeTimeline.transitions[2].alignment == "end-black",
              "la fermeture : 110 à 160, end-black");
        const std::string edgeXml = WriteResolveXml(edgeTimeline);
        Check(edgeXml.find("<alignment>start-black</alignment>") !=
                      std::string::npos &&
                  edgeXml.find("<alignment>end-black</alignment>") !=
                      std::string::npos,
              "les deux alignements sont épelés dans le XML");

        // Un fondu par une couleur au milieu d'une piste, ou par une autre
        // couleur que le noir : refusés nommément plutôt qu'envoyés comme
        // un fondu enchaîné que Resolve rendrait autrement.
        Document middle = Montage();
        middle.sequence.transitions[0].type = "dip_to_color";
        middle.sequence.transitions[0].duration = {4, 25};
        ResolveXmlTimeline refused;
        Check(!BuildResolveXmlTimeline(middle, refused, edgeError) &&
                  edgeError.find("edge of a track") != std::string::npos,
              "un dip au milieu d'une piste est refusé : " + edgeError);

        Document white = edges;
        white.sequence.transitions[1].color = "#ffffff";
        Check(!BuildResolveXmlTimeline(white, refused, edgeError) &&
                  edgeError.find("start-black") != std::string::npos,
              "une ouverture sur autre chose que du noir est refusée : " +
                  edgeError);
    }

    // --- Refus ------------------------------------------------------------
    // Le cas que le ticket nomme : un rush à 50 i/s à vitesse 1 dont l'entrée
    // est une image source impaire. 101 * 25 / 50 = 50,5 : il n'y a pas
    // d'image à écrire, et l'arrondir déplacerait la plage d'une demi-image.
    Document odd = Montage();
    odd.sequence.tracks[0].clips[2].source_in = {101, 50};
    ResolveXmlTimeline rejected;
    Check(!BuildResolveXmlTimeline(odd, rejected, error) &&
              error.find("01K30000000000000000000022") != std::string::npos &&
              error.find("whole sequence frame") != std::string::npos,
          "une entrée source impaire à vitesse 1 sur du 50 i/s est refusée, "
          "et le refus nomme le plan : " +
              error);

    Document offGrid = Montage();
    offGrid.sequence.tracks[0].clips[1].timeline_in = {1, 50};
    Check(!BuildResolveXmlTimeline(offGrid, rejected, error) &&
              error.find("01K30000000000000000000021") != std::string::npos,
          "une position entre deux images de séquence est refusée : " + error);

    Document unknown = Montage();
    unknown.library.clear();
    Check(!BuildResolveXmlTimeline(unknown, rejected, error) &&
              error.find("not in the library") != std::string::npos,
          "un média absent de la médiathèque est refusé : " + error);

    Document fractional = Montage();
    fractional.sequence.frame_rate = {30000, 1001};
    Check(!BuildResolveXmlTimeline(fractional, rejected, error) &&
              error.find("whole number of frames") != std::string::npos,
          "une cadence non entière est refusée plutôt qu'arrondie : " + error);

    Document silent;
    silent.sequence.frame_rate = {25, 1};
    Check(!BuildResolveXmlTimeline(silent, rejected, error),
          "une timeline sans plan vidéo n'a rien à écrire");

    // --- Le fichier -------------------------------------------------------
    const std::string xml = WriteResolveXml(timeline);
    Check(xml.find("<!DOCTYPE xmeml>") != std::string::npos &&
              xml.find("<xmeml version=\"5\">") != std::string::npos,
          "le fichier s'annonce comme un xmeml v5");
    Check(xml.find("cutmachine.resolve-xml.v1") != std::string::npos,
          "le fichier nomme son schéma");
    // Un commentaire XML ne peut pas contenir deux tirets : le fichier était
    // refusé par tout parseur avant d'arriver à Resolve.
    Check(xml.find("<!--") != std::string::npos &&
              xml.find("--", xml.find("<!--") + 4) ==
                  xml.find("-->", xml.find("<!--")),
          "le commentaire d'en-tête ne contient pas de double tiret");
    Check(xml.find("<in>100</in>") != std::string::npos &&
              xml.find("<out>150</out>") != std::string::npos,
          "les bornes source sortent dans les unités du XML");
    Check(xml.find("<start>0</start>") != std::string::npos &&
              xml.find("<end>50</end>") != std::string::npos,
          "la position sort en images de séquence");
    Check(xml.find("<effectid>timeremap</effectid>") != std::string::npos &&
              xml.find("<value>50</value>") != std::string::npos,
          "la vitesse passe par le filtre Time Remap");
    Check(xml.find("<value>200</value>") != std::string::npos,
          "l'accéléré aussi");
    Check(xml.find("<alignment>center</alignment>") != std::string::npos &&
              xml.find("<name>Cross Dissolve</name>") != std::string::npos,
          "le fondu enchaîné sort en transitionitem");
    // Resolve apparie un média par son timecode de départ, que le document ne
    // porte pas : le moteur écrit un timecode qui ne peut rien apparier, pour
    // qu'un XML non complété arrive hors ligne plutôt que sur un autre rush.
    Check(xml.find(std::string("<string>") + kUnknownStartTimecode +
                   "</string>") != std::string::npos,
          "le timecode de départ est un marqueur, pas 00:00:00:00");
    Check(xml.find("<file id=\"file-1\">") != std::string::npos &&
              xml.find("<file id=\"file-1\"/>") != std::string::npos,
          "un rush relu par un second plan est référencé par son id, jamais "
          "décrit deux fois");
    size_t tracks = 0;
    for (size_t at = xml.find("<track>"); at != std::string::npos;
         at = xml.find("<track>", at + 1)) {
        ++tracks;
    }
    Check(tracks == 2, "une couche de recouvrement fait une seconde piste");

    // Un montage sans vitesse n'écrit aucun filtre : le fichier d'un projet
    // qui ne se sert pas de R3 ne porte pas sa trace.
    Document plain;
    plain.sequence.frame_rate = {25, 1};
    plain.sequence.width = 1920;
    plain.sequence.height = 1080;
    plain.library = {Media("01K30000000000000000000001", "A.MP4", 25, 500)};
    DocumentTrack single;
    single.id = "01K30000000000000000000010";
    single.kind = "video";
    single.index = 0;
    single.clips = {Clip("01K30000000000000000000020",
                         "01K30000000000000000000001", {0, 25}, {75, 25},
                         {50, 25})};
    plain.sequence.tracks = {single};
    ResolveXmlTimeline simple;
    Check(BuildResolveXmlTimeline(plain, simple, error),
          "un montage sans vitesse s'écrit aussi : " + error);
    const std::string plainXml = WriteResolveXml(simple);
    Check(plainXml.find("timeremap") == std::string::npos &&
              plainXml.find("<transitionitem>") == std::string::npos,
          "ni filtre ni transition pour un montage qui n'en a pas");
    Check(plainXml.find("<in>75</in>") != std::string::npos,
          "à vitesse 1 et cadence égale, l'image source est son propre "
          "numéro");

    Check(plainXml.find("<effectid>basic</effectid>") == std::string::npos,
          "pas de Basic Motion pour un plan qui ne cadre rien");

    // --- R9 après R1 : le cadrage, par Basic Motion -----------------------
    // Le zoom 1,0 de Resolve dépend du réglage du projet ; le document ne le
    // connaît pas, donc un plan cadré est refusé tant qu'il n'est pas déclaré.
    Document framed = plain;
    DocumentClip& shot = framed.sequence.tracks[0].clips[0];
    shot.framing.mode = FramingMode::Fill;
    shot.framing.position_x = {1, 10};
    shot.framing.position_y = {-1, 20};
    ResolveXmlTimeline withFraming;
    Check(!BuildResolveXmlTimeline(framed, withFraming, error) &&
              error.find("resolve_scaling") != std::string::npos,
          "un plan cadré sans réglage Resolve déclaré est refusé : " + error);
    // Rush vertical 2160×3840 dans une séquence 1920×1080, en « fill », et
    // Resolve qui ajuste : le zoom porte le rapport des deux échelles,
    // max(W/Wd, H/Hd) / min(W/Wd, H/Hd) = 7372800 / 2332800.
    Check(BuildResolveXmlTimeline(framed, withFraming, error,
                                  ResolveScaling::Fit),
          "un plan cadré s'écrit : " + error);
    const std::string framedXml = WriteResolveXml(withFraming);
    Check(withFraming.clips.size() == 1 &&
              withFraming.clips[0].motion.size() == 1,
          "un cadrage fixe est une seule valeur, sans clé");
    Check(framedXml.find("<effectid>basic</effectid>") != std::string::npos &&
              framedXml.find("<value>316.049382</value>") != std::string::npos,
          "l'échelle est 100 fois le rapport remplir/ajuster, tronquée à six "
          "décimales");
    Check(framedXml.find("<horiz>0.316049</horiz>") != std::string::npos &&
              framedXml.find("<vert>-0.05</vert>") != std::string::npos,
          "le centre est rapporté à l'image ajustée, avant le zoom du plan");
    Check(framedXml.find("<keyframe>") == std::string::npos,
          "aucune clé pour un cadrage fixe");
    // Resolve qui remplit (le cas mesuré sur les livraisons 9:16) : un
    // cadrage « fill » à l'échelle 1 est son zoom 1,0 ; un cadrage « fit »
    // en est le rapport inverse.
    ResolveXmlTimeline filled;
    Check(
        BuildResolveXmlTimeline(framed, filled, error, ResolveScaling::Fill) &&
            filled.clips[0].motion.size() == 1 &&
            FormatResolveXmlFraction(filled.clips[0].motion[0].scale_percent) ==
                "100",
        "« fill » dans un Resolve qui remplit : zoom 100 % : " + error);
    Check(
        FormatResolveXmlFraction(filled.clips[0].motion[0].center_x) == "0.1" &&
            FormatResolveXmlFraction(filled.clips[0].motion[0].center_y) ==
                "-0.01582",
        "le centre d'un Resolve qui remplit tient compte de la hauteur "
        "débordante");
    Document landscape = framed;
    landscape.sequence.width = 1080;
    landscape.sequence.height = 1920;
    landscape.library[0].width = 3840;
    landscape.library[0].height = 2160;
    ResolveXmlTimeline landscapeXml;
    Check(BuildResolveXmlTimeline(landscape, landscapeXml, error,
                                  ResolveScaling::Fit) &&
              FormatResolveXmlFraction(
                  landscapeXml.clips[0].motion[0].center_x) == "0.1" &&
              FormatResolveXmlFraction(
                  landscapeXml.clips[0].motion[0].center_y) == "-0.158024",
          "un rush horizontal ajusté dans du vertical corrige l'autre axe");
    Document fitted = framed;
    fitted.sequence.tracks[0].clips[0].framing.mode = FramingMode::Fit;
    Check(
        BuildResolveXmlTimeline(fitted, filled, error, ResolveScaling::Fill) &&
            FormatResolveXmlFraction(filled.clips[0].motion[0].scale_percent) ==
                "31.640625",
        "« fit » dans un Resolve qui remplit : 100 × 2332800 / 7372800 : " +
            (filled.clips.empty()
                 ? error
                 : FormatResolveXmlFraction(
                       filled.clips[0].motion[0].scale_percent)));
    ResolveScaling parsed = ResolveScaling::Undeclared;
    Check(ParseResolveScaling("fill", parsed) &&
              parsed == ResolveScaling::Fill &&
              !ParseResolveScaling("crop", parsed),
          "le réglage se lit « fit » ou « fill », rien d'autre");
    Check(FormatResolveXmlFraction({-1, 3}) == "-0.333333" &&
              FormatResolveXmlFraction({0, 7}) == "0" &&
              FormatResolveXmlFraction({23, 2}) == "11.5",
          "les fractions s'écrivent sans double");

    // --- R9 après R2 : les clés ---------------------------------------------
    // Zoom de 1 à 23/20 sur 12 images, en « fit », sur un plan de 50 images
    // ouvert sur l'image source 75 : une clé par image qui change, plus la
    // dernière, et `<when>` dans la base de `<in>`.
    Document animated = plain;
    animated.sequence.tracks[0].clips[0].animation = ZoomTrack();
    ResolveXmlTimeline withKeys;
    Check(
        BuildResolveXmlTimeline(animated, withKeys, error, ResolveScaling::Fit),
        "un plan animé s'écrit : " + error);
    const std::string keyedXml = WriteResolveXml(withKeys);
    Check(withKeys.clips.size() == 1 && withKeys.clips[0].motion.size() == 14,
          "treize images de rampe et la dernière du plan : " +
              std::to_string(withKeys.clips.empty()
                                 ? 0
                                 : withKeys.clips[0].motion.size()));
    Check(keyedXml.find("<when>75</when>") != std::string::npos &&
              keyedXml.find("<when>87</when>") != std::string::npos &&
              keyedXml.find("<when>124</when>") != std::string::npos,
          "les clés sont en images de la base de <in> : 75, 87, 124");
    Check(keyedXml.find("<value>115</value>") != std::string::npos &&
              keyedXml.find("<value>107.5</value>") != std::string::npos,
          "les valeurs sont celles que le moteur échantillonne");
    animated.sequence.tracks[0].clips[0].animation[0].parameter =
        ClipAnimatedParameter::FramingPositionX;
    Check(BuildResolveXmlTimeline(animated, withKeys, error,
                                  ResolveScaling::Fit) &&
              FormatResolveXmlFraction(withKeys.clips[0].motion[0].center_x) ==
                  "3.160493" &&
              FormatResolveXmlFraction(withKeys.clips[0].motion[12].center_x) ==
                  "3.634567",
          "les positions animées suivent la même conversion que le cadrage "
          "fixe");

    // --- Refus nommés
    // ---------------------------------------------------------
    Document cropped = framed;
    cropped.sequence.tracks[0].clips[0].framing.crop_left = {1, 8};
    ResolveXmlTimeline refused;
    Check(!BuildResolveXmlTimeline(cropped, refused, error,
                                   ResolveScaling::Fit) &&
              error.find("crops its source") != std::string::npos,
          "un recadrage est refusé nommément : " + error);
    Document translucent = plain;
    translucent.sequence.tracks[0].clips[0].opacity = {1, 2};
    Check(!BuildResolveXmlTimeline(translucent, refused, error) &&
              error.find("not fully opaque") != std::string::npos,
          "une opacité est refusée nommément, au lieu d'arriver opaque : " +
              error);
    Document graded = plain;
    ClipEffect exposure;
    exposure.id = "01K30000000000000000000E01";
    exposure.type = "color.exposure";
    exposure.params = {{"amount", {1, 2}}};
    graded.sequence.tracks[0].clips[0].effects = {exposure};
    ResolveXmlTimeline counted;
    Check(BuildResolveXmlTimeline(graded, counted, error) &&
              counted.grades_not_sent == 1,
          "un étalonnage n'est pas transmis, et il est compté : " + error);
    // EF3 -- docs/roadmap/EFFETS-2026-10.md. Nor a blur, counted the same
    // way rather than refused.
    Document blurred = plain;
    blurred.sequence.tracks[0].clips[0].blur = {10, 1};
    ResolveXmlTimeline blurCounted;
    Check(BuildResolveXmlTimeline(blurred, blurCounted, error) &&
              blurCounted.blurs_not_sent == 1 &&
              blurCounted.grades_not_sent == 0,
          "un flou n'est pas transmis, et il est compté : " + error);
    // PA8 -- docs/roadmap/PARITE-2026-11.md. Nor a mask.
    Document masked = plain;
    masked.sequence.tracks[0].clips[0].mask.shape = kClipMaskRectangle;
    ResolveXmlTimeline maskCounted;
    Check(BuildResolveXmlTimeline(masked, maskCounted, error) &&
              maskCounted.masks_not_sent == 1 &&
              maskCounted.blurs_not_sent == 0,
          "un masque n'est pas transmis, et il est compté : " + error);

    if (failures != 0) {
        std::cerr << failures << " assertion(s) en échec\n";
        return 1;
    }
    std::cout << "All resolve XML export tests passed\n";
    return 0;
}
