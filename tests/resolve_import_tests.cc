#include "Cli.h"
#include "Document.h"
#include "EditLog.h"
#include "Operations.h"
#include "ProjectStorage.h"
#include "ResolveExport.h"
#include "ResolveImport.h"
#include "ResolveXmlExport.h"

#include "ClipSpeed.h"
#include "Ulid.h"

#include "resolve_database_fixture.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

std::string Quote(const std::filesystem::path& path) {
    std::string result = "'";
    for (char character : path.string()) {
        if (character == '\'')
            result += "'\\''";
        else
            result += character;
    }
    return result + "'";
}

void CheckRejected(const std::string& json, const std::string& label) {
    ResolveManifest manifest;
    std::string error;
    Check(!ParseResolveManifest(json, manifest, error),
          "le manifeste doit être refusé : " + label);
    Check(!error.empty(), "un refus nomme son motif : " + label);
}

void CheckTimelineRejected(const std::string& json, const std::string& label) {
    ResolveTimelineRead manifest;
    std::string error;
    Check(!ParseResolveTimelineManifest(json, manifest, error),
          "le manifeste de timeline doit être refusé : " + label);
    Check(!error.empty(), "un refus nomme son motif : " + label);
}

// A library with `count` rushes, every one at `rate` i/s and `frames` long.
// No FFmpeg: R10 links to media the library already holds, so a synthetic
// library is the whole input the planner needs.
Document SyntheticLibrary(int count, int32_t rate, int64_t frames) {
    Document document;
    for (int index = 0; index < count; ++index) {
        LibraryMedia media;
        media.id = GenerateUlid();
        media.filename = "R" + std::to_string(index) + ".MP4";
        media.path = "/rushes/" + media.filename;
        media.codec = "h264";
        media.width = 1920;
        media.height = 1080;
        media.orientation = "landscape";
        media.rate = {rate, 1};
        media.duration = {frames, rate};
        document.library.push_back(media);
        document.sources.push_back(
            {media.id, media.path, media.rate, media.duration});
    }
    return document;
}

struct TimelineItemFixture {
    std::string name;
    int64_t record_start = 0;
    int64_t record_end = 0;
    int64_t duration = 0;
    int64_t source_start = 0;
    int64_t fusion_comps = 0;
    std::string path;
    // Already-serialized "key":"value" pairs, because a property travels as
    // text and never as a JSON float.
    std::string properties;
    // R10 -- the facts an exact speed comes from. The defaults describe a
    // clip at speed 1 over a 50 i/s media in a 25 i/s timeline, which is what
    // every fixture that does not care about speed wants: left + span + right
    // then equals media_frames * rate / media_rate. Set `speed_unmeasurable`
    // to check that a clip whose speed cannot be read is refused rather than
    // assumed normal.
    int64_t left_offset = 0;
    int64_t right_offset = -1;  // -1: derived so the clip reads at speed 1.
    int64_t media_frames = 100000;
    int32_t media_rate = 50;
    std::string speed_unmeasurable;
};

// R10 -- one transition item, as GetItemListInTrack hands it back: a name
// that is its kind, bounds that overlap the two clips it joins, and no media
// at all. The defaults are the measured shape of a 12-frame centred cross
// dissolve on a cut at 100.
struct TimelineTransitionFixture {
    std::string name = "Cross Dissolve";
    int64_t record_start = 0;
    int64_t record_end = 0;
};

std::string TimelineManifest(
    const std::string& name, int32_t rate,
    const std::vector<TimelineItemFixture>& items,
    const std::vector<TimelineTransitionFixture>& transitions = {}) {
    std::ostringstream json;
    json << R"({"schema":"cutmachine.resolve-timeline-read.v1",)"
         << R"("project":"P","name":")" << name << R"(","frame_rate":)" << rate
         << R"(,"origin":90000,"width":1080,"height":1920,)"
         << R"("unpublished":["la vitesse d'un plan"],)"
         << R"("tracks":[{"kind":"video","index":1,"name":"V1","clips":[)";
    for (size_t index = 0; index < items.size(); ++index) {
        const TimelineItemFixture& item = items[index];
        if (index) json << ',';
        json << R"({"name":")" << item.name << R"(","record_start":)"
             << item.record_start << R"(,"record_end":)" << item.record_end
             << R"(,"duration":)" << item.duration << R"(,"source_start":)"
             << item.source_start << R"(,"fusion_comps":)" << item.fusion_comps
             << R"(,"path":")" << item.path << '"';
        if (!item.speed_unmeasurable.empty()) {
            json << R"(,"speed_unmeasurable":")" << item.speed_unmeasurable
                 << '"';
        } else {
            const int64_t span = item.record_end - item.record_start;
            const int64_t whole = item.media_frames * rate / item.media_rate;
            const int64_t right = item.right_offset >= 0
                                      ? item.right_offset
                                      : whole - item.left_offset - span;
            json << R"(,"left_offset":)" << item.left_offset
                 << R"(,"right_offset":)" << right << R"(,"media_frames":)"
                 << item.media_frames << R"(,"media_rate":)" << item.media_rate;
        }
        if (item.path.empty()) json << R"(,"no_file_reason":"Compound Clip")";
        if (!item.properties.empty())
            json << R"(,"properties":{)" << item.properties << '}';
        json << '}';
    }
    json << R"(],"transitions":[)";
    for (size_t index = 0; index < transitions.size(); ++index) {
        const TimelineTransitionFixture& transition = transitions[index];
        if (index) json << ',';
        json << R"({"name":")" << transition.name << R"(","record_start":)"
             << transition.record_start << R"(,"record_end":)"
             << transition.record_end << R"(,"duration":)"
             << (transition.record_end - transition.record_start) << '}';
    }
    json << "]}]}";
    return json.str();
}

const DocumentBin* FindBin(const Document& document, const std::string& name) {
    for (const DocumentBin& bin : document.bins)
        if (bin.name == name) return &bin;
    return nullptr;
}

}  // namespace

int main() {
    // --- Parsing -----------------------------------------------------------
    const std::string valid = R"({
        "schema": "cutmachine.resolve-manifest.v1",
        "project": "PROJET",
        "resolve_version": "20.3.1.6",
        "bins": [
            {"key": "b2", "name": "Jour 1", "parent_key": "b1"},
            {"key": "b1", "name": "1_RUSHES", "parent_key": ""}
        ],
        "clips": [
            {"path": "/rushes/C8015.MP4", "name": "C8015.MP4",
             "bin_key": "b2", "resolve_uid": "5d45"},
            {"path": "/rushes/C8035.MP4", "name": "C8035.MP4", "bin_key": ""}
        ]
    })";
    ResolveManifest manifest;
    std::string error;
    Check(ParseResolveManifest(valid, manifest, error),
          "un manifeste valide est accepté : " + error);
    Check(manifest.project == "PROJET" && manifest.bins.size() == 2 &&
              manifest.clips.size() == 2,
          "le manifeste conserve projet, chutiers et rushes");
    Check(manifest.clips[0].resolve_uid == "5d45",
          "l'identifiant Resolve est conservé pour le diagnostic");

    CheckRejected(R"({"schema":"autre.v1"})", "schéma inconnu");
    CheckRejected(R"({"project":"x"})", "schéma absent");
    CheckRejected(R"({"schema":"cutmachine.resolve-manifest.v1","bins":[
        {"key":"b1","name":"A"},{"key":"b1","name":"B"}]})",
                  "clé dupliquée");
    CheckRejected(R"({"schema":"cutmachine.resolve-manifest.v1","bins":[
        {"key":"b1","name":"A","parent_key":"absent"}]})",
                  "parent inconnu");
    CheckRejected(R"({"schema":"cutmachine.resolve-manifest.v1","bins":[
        {"key":"b1","name":"A","parent_key":"b2"},
        {"key":"b2","name":"B","parent_key":"b1"}]})",
                  "cycle de chutiers");
    CheckRejected(R"({"schema":"cutmachine.resolve-manifest.v1","clips":[
        {"path":"/a.mp4","bin_key":"absent"}]})",
                  "rush dans un chutier absent");
    CheckRejected(R"({"schema":"cutmachine.resolve-manifest.v1","clips":[
        {"path":""}]})",
                  "rush sans chemin");
    CheckRejected(R"({"schema":"cutmachine.resolve-manifest.v1","bins":[
        {"key":"","name":"A"}]})",
                  "clé vide");

    // --- Planning ----------------------------------------------------------
    Document document;
    ResolveImportPlan plan;
    Check(PlanResolveImport(document, manifest, plan, error),
          "un document vierge se planifie : " + error);
    Check(plan.new_bins.size() == 2 && plan.reused_bins == 0,
          "les deux chutiers sont à créer");
    Check(plan.new_bins[0].name == "1_RUSHES" &&
              plan.new_bins[0].parent_id.empty(),
          "le parent est créé avant l'enfant, à la racine");
    Check(plan.new_bins[1].name == "Jour 1" &&
              plan.new_bins[1].parent_id == plan.new_bins[0].bin_id,
          "l'enfant pointe vers le ULID de son parent");
    Check(plan.bin_ids.count("") == 1 && plan.bin_ids.at("").empty(),
          "la racine du manifeste reste le chutier vide");

    // Applying the plan then re-planning must create nothing: a second import
    // of a Media Pool that grew adds only what is new.
    for (const AddBinOperation& operation : plan.new_bins)
        document.bins.push_back(
            {operation.bin_id, operation.name, operation.parent_id});
    ResolveImportPlan replan;
    Check(PlanResolveImport(document, manifest, replan, error),
          "un second passage se planifie : " + error);
    Check(replan.new_bins.empty() && replan.reused_bins == 2,
          "un second import ne duplique aucun chutier");
    Check(replan.bin_ids.at("b2") == plan.new_bins[1].bin_id,
          "le chutier réutilisé garde son identité");

    // A homonym under a different parent is a different bin.
    Document twoParents;
    ResolveManifest homonyms;
    Check(ParseResolveManifest(
              R"({"schema":"cutmachine.resolve-manifest.v1","bins":[
                  {"key":"a","name":"Jour 01"},
                  {"key":"b","name":"B"},
                  {"key":"c","name":"Jour 01","parent_key":"b"}]})",
              homonyms, error),
          "manifeste homonyme accepté : " + error);
    ResolveImportPlan homonymPlan;
    Check(PlanResolveImport(twoParents, homonyms, homonymPlan, error),
          "manifeste homonyme planifié : " + error);
    Check(homonymPlan.new_bins.size() == 3,
          "deux chutiers homonymes sous des parents différents coexistent");

    // --- End to end --------------------------------------------------------
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / (GenerateUlid() + "-resolve");
    const std::filesystem::path media = root / "rushes";
    std::filesystem::create_directories(media);
    const std::filesystem::path first = media / "C8015.MP4";
    const std::filesystem::path second = media / "C8035.MP4";
    for (const std::filesystem::path& path : {first, second}) {
        const std::string generate =
            Quote(FFMPEG_EXECUTABLE) +
            " -hide_banner -loglevel error -f lavfi "
            "-i 'color=c=black:s=64x32:r=25:d=0.4' -c:v mpeg4 " +
            Quote(path);
        Check(std::system(generate.c_str()) == 0,
              "FFmpeg génère le rush de test " + path.filename().string());
    }

    std::string projectPath;
    Check(CreatePortableProject((root / "Resolve.cutmachine-project").string(),
                                Project("Resolve"), projectPath, error),
          "le paquet projet se crée : " + error);

    const std::filesystem::path manifestPath = root / "manifest.json";
    {
        std::ofstream out(manifestPath);
        out << R"({"schema":"cutmachine.resolve-manifest.v1",)"
            << R"("project":"Resolve","bins":[)"
            << R"({"key":"b1","name":"1_RUSHES","parent_key":""},)"
            << R"({"key":"b2","name":"Jour 1","parent_key":"b1"}],)"
            << R"("clips":[{"path":")" << first.string()
            << R"(","name":"C8015.MP4","bin_key":"b2"},)" << R"({"path":")"
            << second.string() << R"(","name":"C8035.MP4","bin_key":"b1"}]})";
    }

    std::string output;
    Check(ImportResolveCommand(projectPath, manifestPath.string(), output) == 0,
          "l'import Resolve réussit : " + output);
    Check(output.find("\"added\":2") != std::string::npos &&
              output.find("\"bins_created\":2") != std::string::npos,
          "l'import rapporte deux rushes et deux chutiers : " + output);

    Project reloaded;
    Check(LoadStoredProject(projectPath, reloaded, error),
          "le projet se recharge : " + error);
    Document stored = reloaded.MakeActiveDocument();
    Check(stored.library.size() == 2,
          "les deux rushes sont dans la médiathèque");
    const DocumentBin* rushes = FindBin(stored, "1_RUSHES");
    const DocumentBin* day = FindBin(stored, "Jour 1");
    Check(rushes != nullptr && day != nullptr, "les deux chutiers existent");
    if (rushes != nullptr && day != nullptr) {
        Check(rushes->parent_id.empty() && day->parent_id == rushes->id,
              "la hiérarchie Resolve est reproduite");
        for (const LibraryMedia& item : stored.library) {
            if (item.filename == "C8015.MP4")
                Check(item.bin_id == day->id,
                      "le rush du sous-chutier est classé dans Jour 1");
            if (item.filename == "C8035.MP4")
                Check(item.bin_id == rushes->id,
                      "le rush du chutier parent est classé dans 1_RUSHES");
        }
        Check(stored.library[0].rate.den != 0 &&
                  stored.library[0].duration.rate != 0,
              "la cadence vient de la sonde FFmpeg, pas du manifeste");
    }

    // Re-importing the same manifest must be a no-op, not a duplication.
    std::string secondOutput;
    Check(ImportResolveCommand(projectPath, manifestPath.string(),
                               secondOutput) == 0,
          "le second import réussit : " + secondOutput);
    Check(secondOutput.find("\"added\":0") != std::string::npos &&
              secondOutput.find("\"bins_created\":0") != std::string::npos &&
              secondOutput.find("\"bins_reused\":2") != std::string::npos,
          "le second import ne crée rien : " + secondOutput);
    Project again;
    Check(LoadStoredProject(projectPath, again, error),
          "le projet se recharge après le second import : " + error);
    Check(again.MakeActiveDocument().library.size() == 2 &&
              again.MakeActiveDocument().bins.size() == 2,
          "aucun doublon après un second import");

    // A rush whose file is gone must be named, not just counted: on a Media
    // Pool of hundreds, "skipped: 3" is not a report.
    const std::filesystem::path offlinePath = root / "offline.json";
    {
        std::ofstream out(offlinePath);
        out << R"({"schema":"cutmachine.resolve-manifest.v1","clips":[)"
            << R"({"path":")" << (media / "C9999.MP4").string()
            << R"(","name":"C9999.MP4"}]})";
    }
    std::string offlineOutput;
    Check(ImportResolveCommand(projectPath, offlinePath.string(),
                               offlineOutput) == 0,
          "un rush hors ligne n'arrête pas le lot : " + offlineOutput);
    Check(offlineOutput.find("\"skipped\":1") != std::string::npos &&
              offlineOutput.find("C9999.MP4") != std::string::npos,
          "le rush hors ligne est nommé dans errors : " + offlineOutput);
    Check(secondOutput.find("\"errors\":[]") != std::string::npos,
          "un rush déjà présent n'est pas rapporté comme erreur : " +
              secondOutput);

    std::string missing;
    Check(ImportResolveCommand(projectPath, (root / "absent.json").string(),
                               missing) != 0,
          "un manifeste absent échoue");

    // --- R10 : relire une timeline ----------------------------------------
    // Parsing, refus et planification sont purs : ni Resolve, ni FFmpeg, ni
    // projet sur le disque. Les manifestes ci-dessous sont synthétiques.
    {
        const std::string timelineJson = R"({
            "schema": "cutmachine.resolve-timeline-read.v1",
            "project": "P", "resolve_version": "20.3.1.6",
            "name": "MONTAGE_V2", "frame_rate": 25, "origin": 90000,
            "width": 1080, "height": 1920,
            "unpublished": ["la vitesse d'un plan"],
            "tracks": [
              {"kind": "video", "index": 1, "name": "V1", "clips": [
                {"name": "A", "record_start": 0, "record_end": 40,
                 "duration": 40, "source_start": 12, "fusion_comps": 0,
                 "path": "/rushes/R0.MP4",
                 "properties": {"ZoomX": "1.000", "Pan": "0"}}]},
              {"kind": "audio", "index": 1, "clips": [
                {"name": "A", "record_start": 0, "record_end": 40,
                 "duration": 40, "source_start": 12,
                 "path": "/rushes/R0.MP4"}]}
            ]})";
        ResolveTimelineRead read;
        Check(ParseResolveTimelineManifest(timelineJson, read, error),
              "un manifeste de timeline valide est accepté : " + error);
        Check(read.name == "MONTAGE_V2" && read.frame_rate == 25 &&
                  read.width == 1080 && read.height == 1920 &&
                  read.origin == 90000,
              "le manifeste conserve nom, cadence, définition et origine");
        Check(read.tracks.size() == 2 && read.tracks[0].kind == "video" &&
                  read.tracks[1].kind == "audio" &&
                  read.tracks[0].clips.size() == 1,
              "les pistes vidéo et audio sont relues");
        Check(read.tracks[0].clips[0].properties.size() == 2 &&
                  read.unpublished.size() == 1,
              "propriétés et non-publiés traversent le parsing");

        CheckTimelineRejected(R"({"schema":"cutmachine.resolve-timeline.v1"})",
                              "le schéma retiré du renvoi n'est pas celui de "
                              "la relecture");
        CheckTimelineRejected(
            R"({"schema":"cutmachine.resolve-timeline-read.v1","name":"T",
                "frame_rate":25,"width":1080,"height":1920})",
            "timeline sans pistes");
        CheckTimelineRejected(
            R"({"schema":"cutmachine.resolve-timeline-read.v1","name":"T",
                "frame_rate":0,"width":1080,"height":1920,
                "tracks":[{"kind":"video","index":1,"clips":[]}]})",
            "cadence nulle");
        CheckTimelineRejected(
            R"({"schema":"cutmachine.resolve-timeline-read.v1","name":"T",
                "frame_rate":25,"width":1080,"height":1920,
                "tracks":[{"kind":"subtitle","index":1,"clips":[]}]})",
            "piste de type inconnu");
        CheckTimelineRejected(
            R"({"schema":"cutmachine.resolve-timeline-read.v1","name":"T",
                "frame_rate":25,"width":1080,"height":1920,"tracks":[
                {"kind":"video","index":1,"clips":[]},
                {"kind":"video","index":1,"clips":[]}]})",
            "piste dupliquée");
        // Un flottant JSON n'entre pas : le format canonique les refuse, et
        // une valeur de cadrage se compare en texte décimal exact.
        CheckTimelineRejected(
            R"({"schema":"cutmachine.resolve-timeline-read.v1","name":"T",
                "frame_rate":25,"width":1080,"height":1920,"tracks":[
                {"kind":"video","index":1,"clips":[{"record_start":0,
                 "record_end":10,"source_start":0,"path":"/a.mp4",
                 "properties":{"ZoomX":1.2}}]}]})",
            "propriété en flottant JSON");
        CheckTimelineRejected(
            R"({"schema":"cutmachine.resolve-timeline-read.v1","name":"T",
                "frame_rate":25,"width":1080,"height":1920,"tracks":[
                {"kind":"video","index":1,"clips":[{"record_end":10,
                 "source_start":0,"path":"/a.mp4"}]}]})",
            "plan sans record_start");
    }

    // Critère 2 : vitesse, cadrage, effet, fichier absent et média inconnu
    // sont refusés nommément, et rien n'est planifié.
    {
        const Document library = SyntheticLibrary(2, 25, 500);
        std::vector<TimelineItemFixture> items;
        items.push_back({"CADRAGE", 0, 40, 40, 0, 0, "/rushes/R0.MP4",
                         "\"ZoomX\":\"1.200\""});
        // Une vitesse s'importe depuis R3 ; ce qui reste refusé, c'est une
        // vitesse que le pont n'a pas pu mesurer — la supposer normale est
        // ce qui décale toutes les images du plan sans le dire.
        TimelineItemFixture unmeasurable{"VITESSE",        40, 80, 20, 0, 0,
                                         "/rushes/R0.MP4", ""};
        unmeasurable.speed_unmeasurable = "GetLeftOffset absent";
        items.push_back(unmeasurable);
        items.push_back({"FUSION", 80, 120, 40, 0, 2, "/rushes/R0.MP4", ""});
        items.push_back({"COMPOSE", 120, 160, 40, 0, 0, "", ""});
        items.push_back({"INCONNU", 160, 200, 40, 0, 0, "/ailleurs/X.MP4", ""});
        items.push_back({"BON", 200, 240, 40, 0, 0, "/rushes/R1.MP4", ""});
        ResolveTimelineRead read;
        Check(ParseResolveTimelineManifest(TimelineManifest("REFUS", 25, items),
                                           read, error),
              "le manifeste des refus se parse : " + error);
        ResolveTimelineImportPlan plan;
        Check(!PlanResolveTimelineImport(library, "/projet", read, plan, error),
              "une timeline non exprimable est refusée");
        Check(plan.refusals.size() == 5,
              "cinq plans refusés, un par motif : " +
                  std::to_string(plan.refusals.size()));
        const std::string named =
            DescribeResolveTimelineRefusals(plan.refusals);
        Check(named.find("ZoomX=1.200") != std::string::npos &&
                  named.find("cadrage") != std::string::npos,
              "le cadrage est refusé nommément : " + named);
        Check(named.find("vitesse non mesurable") != std::string::npos &&
                  named.find("VITESSE") != std::string::npos,
              "la vitesse est refusée nommément : " + named);
        Check(named.find("Fusion") != std::string::npos,
              "l'effet Resolve est refusé nommément : " + named);
        Check(named.find("aucun fichier") != std::string::npos,
              "le clip sans fichier est refusé nommément : " + named);
        Check(named.find("/ailleurs/X.MP4") != std::string::npos &&
                  named.find("bibliothèque") != std::string::npos,
              "le média absent de la bibliothèque est refusé nommément : " +
                  named);
        Check(error == named, "le refus de la planification est cette liste");
        // Le plan porte encore le plan valide, mais la commande n'écrit rien :
        // c'est le refus qui décide, pas le contenu du plan.
        Check(plan.clips.size() == 1,
              "le plan exprimable reste visible dans le plan refusé");
    }

    // R10 + R3 -- un ralenti mesuré traverse jusqu'au document. Les nombres
    // sont ceux d'un vrai ralenti relevé dans Resolve : un rush de
    // 456 images à 50 i/s dans une séquence à 25 i/s, 100 images de timeline,
    // marges 258 et 98. Le total fait 456 quand le même média n'occuperait
    // que 228 images à vitesse 1, d'où 228/456 = 1/2.
    {
        const Document library = SyntheticLibrary(1, 25, 500);
        TimelineItemFixture ralenti{"C1108",          0, 100, 100, 0, 0,
                                    "/rushes/R0.MP4", ""};
        ralenti.left_offset = 258;
        ralenti.right_offset = 98;
        ralenti.media_frames = 456;
        ralenti.media_rate = 50;
        ResolveTimelineRead read;
        Check(ParseResolveTimelineManifest(
                  TimelineManifest("LENT", 25, {ralenti}), read, error),
              "le manifeste d'un ralenti se parse : " + error);
        ResolveTimelineImportPlan plan;
        Check(PlanResolveTimelineImport(library, "/projet", read, plan, error),
              "un ralenti mesuré n'est plus refusé : " + error);
        Check(plan.clips.size() == 1 && plan.clip_speeds.size() == 1,
              "un plan planifié, une vitesse");
        Check(plan.clip_speeds[0].num == 1 && plan.clip_speeds[0].den == 2,
              "la vitesse mesurée est exactement 1/2 : " +
                  std::to_string(plan.clip_speeds[0].num) + "/" +
                  std::to_string(plan.clip_speeds[0].den));
        Check(plan.retimed_clips == 1, "le rapport compte le plan ralenti");

        // La plage source consommée est la moitié de la durée timeline, et
        // c'est ce que le document doit porter une fois la vitesse posée.
        Document document = library;
        EditLog log;
        EditError editError = EditError::None;
        std::string reason;
        UpdateSequenceOperation sequence = plan.sequence;
        Check(log.Apply(document, sequence, editError, reason),
              "la séquence s'applique : " + reason);
        for (AddTrackOperation track : plan.new_tracks)
            Check(log.Apply(document, track, editError, reason),
                  "la piste s'applique : " + reason);
        InsertClipOperation insert = plan.clips[0];
        Check(log.Apply(document, insert, editError, reason),
              "le plan s'insère : " + reason);
        SetClipSpeedOperation retime;
        retime.clip_id = insert.clip_id;
        retime.speed = plan.clip_speeds[0];
        Check(log.Apply(document, retime, editError, reason),
              "la vitesse se pose : " + reason);
        Check(document.Validate(reason),
              "le document reste valide avec un plan ralenti : " + reason);
        const DocumentClip* clip = document.FindClip(insert.clip_id);
        Check(clip != nullptr && clip->speed.num == 1 && clip->speed.den == 2,
              "le plan du document porte bien la vitesse 1/2");
        // 100 images de timeline à 25 i/s, jouées à 1/2 : 50 images de
        // source, soit deux secondes de média pour quatre de montage.
        Check(clip != nullptr &&
                  ClipSourceSpan(*clip).compare(RationalTime{50, 25}) == 0,
              "un ralenti de moitié consomme la moitié de sa durée en source");
    }

    // R10 -- l'autre sens, mesuré le même jour sur un plan porté à 200 % :
    // marges 0 et 361 pour 83 images de timeline, soit 444 là où le même
    // média occupe 888 images à vitesse 1. La formule n'est pas taillée pour
    // le seul ralenti.
    {
        const Document library = SyntheticLibrary(1, 25, 500);
        TimelineItemFixture rapide{"C1117",          0, 83, 83, 0, 0,
                                   "/rushes/R0.MP4", ""};
        rapide.left_offset = 0;
        rapide.right_offset = 361;
        rapide.media_frames = 1776;
        rapide.media_rate = 50;
        ResolveTimelineRead read;
        Check(ParseResolveTimelineManifest(
                  TimelineManifest("RAPIDE", 25, {rapide}), read, error),
              "le manifeste d'un accéléré se parse : " + error);
        ResolveTimelineImportPlan plan;
        Check(PlanResolveTimelineImport(library, "/projet", read, plan, error),
              "un accéléré mesuré est planifié : " + error);
        Check(plan.clip_speeds.size() == 1 && plan.clip_speeds[0].num == 2 &&
                  plan.clip_speeds[0].den == 1,
              "la vitesse mesurée est exactement 2");
        Check(plan.retimed_clips == 1, "l'accéléré compte comme retimé");
    }

    // Une valeur par défaut, quelle que soit son écriture, ne refuse rien ;
    // une propriété illisible ou inconnue, si.
    {
        const Document library = SyntheticLibrary(1, 25, 500);
        std::vector<TimelineItemFixture> defaults;
        defaults.push_back({"A", 0, 40, 40, 0, 0, "/rushes/R0.MP4",
                            "\"ZoomX\":\"1.000\",\"ZoomY\":\"1\","
                            "\"Pan\":\"-0.0\",\"Opacity\":\"100.00\","
                            "\"CompositeMode\":\"0\""});
        ResolveTimelineRead read;
        ResolveTimelineImportPlan plan;
        Check(ParseResolveTimelineManifest(
                  TimelineManifest("DEFAUTS", 25, defaults), read, error),
              "manifeste des valeurs par défaut : " + error);
        Check(PlanResolveTimelineImport(library, "/projet", read, plan, error),
              "les valeurs par défaut ne refusent rien : " + error);

        std::vector<TimelineItemFixture> unreadable;
        unreadable.push_back(
            {"A", 0, 40, 40, 0, 0, "/rushes/R0.MP4", "\"ZoomX\":\"1,2\""});
        unreadable.push_back(
            {"B", 40, 80, 40, 0, 0, "/rushes/R0.MP4", "\"Inconnue\":\"3\""});
        ResolveTimelineRead second;
        ResolveTimelineImportPlan secondPlan;
        Check(ParseResolveTimelineManifest(
                  TimelineManifest("ILLISIBLE", 25, unreadable), second, error),
              "manifeste des valeurs illisibles : " + error);
        Check(!PlanResolveTimelineImport(library, "/projet", second, secondPlan,
                                         error),
              "une valeur illisible ou une propriété inconnue est refusée");
        Check(error.find("illisible") != std::string::npos &&
                  error.find("inconnue") != std::string::npos,
              "les deux motifs sont nommés : " + error);
    }

    // Un trou n'est pas un refus : timeline_in est absolu. flatten_timeline
    // le refuse parce qu'un rendu à plat a besoin de contiguïté ; pas ici.
    {
        const Document library = SyntheticLibrary(1, 25, 500);
        std::vector<TimelineItemFixture> items;
        items.push_back({"A", 0, 40, 40, 0, 0, "/rushes/R0.MP4", ""});
        items.push_back({"B", 90, 130, 40, 100, 0, "/rushes/R0.MP4", ""});
        ResolveTimelineRead read;
        ResolveTimelineImportPlan plan;
        Check(ParseResolveTimelineManifest(TimelineManifest("TROU", 25, items),
                                           read, error),
              "manifeste avec trou : " + error);
        Check(PlanResolveTimelineImport(library, "/projet", read, plan, error),
              "un trou entre deux plans se relit : " + error);
        Check(plan.clips.size() == 2 && plan.clips[1].timeline_in.value == 90 &&
                  plan.clips[1].timeline_in.rate == 25,
              "le second plan garde sa position absolue");
        Check(plan.new_tracks.size() == 1 && plan.new_tracks[0].kind == "video",
              "la piste manquante est créée");

        // Un chevauchement, lui, n'est pas un montage que le document porte.
        std::vector<TimelineItemFixture> overlap;
        overlap.push_back({"A", 0, 40, 40, 0, 0, "/rushes/R0.MP4", ""});
        overlap.push_back({"B", 30, 70, 40, 0, 0, "/rushes/R0.MP4", ""});
        ResolveTimelineRead second;
        ResolveTimelineImportPlan secondPlan;
        Check(ParseResolveTimelineManifest(
                  TimelineManifest("RECOUVRE", 25, overlap), second, error),
              "manifeste avec chevauchement : " + error);
        Check(!PlanResolveTimelineImport(library, "/projet", second, secondPlan,
                                         error),
              "un chevauchement sur une piste est refusé");
        Check(error.find("chevauchement") != std::string::npos,
              "le chevauchement est nommé : " + error);
    }

    // --- R10 : les fondus enchaînés ---------------------------------------
    // Mesuré sur Resolve 20.3.1 le 22 septembre 2026, sur un montage qui en
    // porte cinq. GetItemListInTrack les publie parmi les plans, nommés
    // d'après leur type, sans MediaPoolItem, et chevauchant les deux plans
    // qu'ils raccordent. Le fondu du milieu de ce montage : coupe à 100,
    // transition 94 → 106, soit 6 images de part et d'autre.
    {
        const Document library = SyntheticLibrary(2, 25, 500);
        std::vector<TimelineItemFixture> items;
        items.push_back({"A", 0, 100, 100, 40, 0, "/rushes/R0.MP4", ""});
        items.push_back({"B", 100, 200, 100, 40, 0, "/rushes/R1.MP4", ""});
        ResolveTimelineRead read;
        ResolveTimelineImportPlan plan;
        Check(ParseResolveTimelineManifest(
                  TimelineManifest("FONDU", 25, items,
                                   {{"Cross Dissolve", 94, 106}}),
                  read, error),
              "le manifeste d'un fondu se parse : " + error);
        Check(read.tracks.size() == 1 && read.tracks[0].transitions.size() == 1,
              "la transition arrive dans sa propre liste, pas parmi les plans");
        Check(read.tracks[0].transitions[0].name == "Cross Dissolve" &&
                  read.tracks[0].transitions[0].record_start == 94 &&
                  read.tracks[0].transitions[0].record_end == 106,
              "le type et les bornes traversent le parsing");
        // Le chevauchement d'une transition n'est pas un refus : c'est ainsi
        // que Resolve la pose. Celui de deux *plans* le reste (plus haut).
        Check(PlanResolveTimelineImport(library, "/projet", read, plan, error),
              "un fondu enchaîné se relit : " + error);
        Check(plan.refusals.empty() && plan.clips.size() == 2,
              "les deux plans passent, le fondu ne les fait pas refuser");
        Check(plan.transitions.size() == 1,
              "le fondu devient une AddTransitionOperation : " +
                  std::to_string(plan.transitions.size()));
        const DocumentTransition& written = plan.transitions[0].transition;
        Check(written.type == "cross_dissolve" &&
                  written.alignment == TransitionAlignment::Center &&
                  written.duration == (RationalTime{12, 25}),
              "type, alignement centré et durée exacte");
        Check(written.left_clip_id == plan.clips[0].clip_id &&
                  written.right_clip_id == plan.clips[1].clip_id &&
                  written.track_id == plan.clips[0].track_id,
              "le fondu nomme par ULID les deux plans que le plan a générés");

        // R5 -- une ouverture et une fermeture au noir : un seul plan,
        // aucune coupe partagée. Depuis R5 le document les porte comme des
        // « dip_to_color » noirs, qui ne lisent aucune poignée. NON MESURÉ
        // devant Resolve : la correspondance est lue des seules bornes.
        {
            ResolveTimelineRead parsed;
            ResolveTimelineImportPlan planned;
            std::string message;
            Check(ParseResolveTimelineManifest(
                      TimelineManifest("BORDS", 25, items,
                                       {{"Cross Dissolve", 0, 40},
                                        {"Cross Dissolve", 150, 200}}),
                      parsed, message),
                  "le manifeste des bords se parse : " + message);
            Check(PlanResolveTimelineImport(library, "/projet", parsed, planned,
                                            message),
                  "les fondus de bord se relisent : " + message);
            Check(planned.transitions.size() == 2,
                  "deux transitions planifiées");
            if (planned.transitions.size() == 2) {
                const DocumentTransition& opening =
                    planned.transitions[0].transition;
                const DocumentTransition& closing =
                    planned.transitions[1].transition;
                Check(
                    opening.type == "dip_to_color" &&
                        opening.color == "#000000" &&
                        opening.left_clip_id.empty() &&
                        opening.right_clip_id == planned.clips[0].clip_id &&
                        opening.alignment == TransitionAlignment::StartAtCut &&
                        opening.duration == (RationalTime{40, 25}),
                    "l'ouverture : 40 images de noir sur le premier plan");
                Check(closing.type == "dip_to_color" &&
                          closing.color == "#000000" &&
                          closing.right_clip_id.empty() &&
                          closing.left_clip_id == planned.clips[1].clip_id &&
                          closing.alignment == TransitionAlignment::EndAtCut &&
                          closing.duration == (RationalTime{50, 25}),
                      "la fermeture : 50 images de noir sur le dernier");
            }
        }

        // Les trois alignements se déduisent des seules bornes. Mesurés tous
        // les trois par un aller-retour réel dans Resolve le 22 septembre.
        const auto alignmentOf = [&](int64_t start, int64_t end,
                                     TransitionAlignment& out) {
            ResolveTimelineRead parsed;
            ResolveTimelineImportPlan planned;
            std::string message;
            if (!ParseResolveTimelineManifest(
                    TimelineManifest("ALIGN", 25, items,
                                     {{"Cross Dissolve", start, end}}),
                    parsed, message))
                return false;
            if (!PlanResolveTimelineImport(library, "/projet", parsed, planned,
                                           message))
                return false;
            if (planned.transitions.size() != 1) return false;
            out = planned.transitions[0].transition.alignment;
            return true;
        };
        TransitionAlignment alignment = TransitionAlignment::Center;
        Check(alignmentOf(100, 112, alignment) &&
                  alignment == TransitionAlignment::StartAtCut,
              "entièrement après la coupe : start_at_cut");
        Check(alignmentOf(88, 100, alignment) &&
                  alignment == TransitionAlignment::EndAtCut,
              "entièrement avant la coupe : end_at_cut");
        // Durée impaire : le document coupe floor/ceil, comme
        // Document::Validate. 13 images font 6 avant et 7 après.
        Check(alignmentOf(94, 107, alignment) &&
                  alignment == TransitionAlignment::Center,
              "13 images en 6/7 restent un fondu centré");
    }

    // Tout ce qui n'est pas un fondu enchaîné entre deux plans est refusé
    // nommément, avec le nom que Resolve lui donne.
    {
        const Document library = SyntheticLibrary(2, 25, 500);
        std::vector<TimelineItemFixture> items;
        items.push_back({"A", 0, 100, 100, 40, 0, "/rushes/R0.MP4", ""});
        items.push_back({"B", 100, 200, 100, 40, 0, "/rushes/R1.MP4", ""});

        const auto refusalFor =
            [&](const std::vector<TimelineTransitionFixture>& transitions,
                const std::vector<TimelineItemFixture>& clips) {
                ResolveTimelineRead parsed;
                ResolveTimelineImportPlan planned;
                std::string message;
                Check(ParseResolveTimelineManifest(
                          TimelineManifest("REFUS", 25, clips, transitions),
                          parsed, message),
                      "le manifeste se parse : " + message);
                Check(!PlanResolveTimelineImport(library, "/projet", parsed,
                                                 planned, message),
                      "la transition est refusée");
                Check(planned.transitions.empty(),
                      "rien n'est planifié pour une transition refusée");
                return message;
            };

        // Un autre type : mesuré, Resolve le nomme « Dip To Color Dissolve »
        // et la relecture le répète tel quel. R5 a donné au document un
        // fondu par une couleur, mais ce que Resolve rapporte du sien --
        // le nom de l'effet, où vit sa couleur -- n'a pas été mesuré, et
        // le deviner rebaptiserait un montage que personne n'a relu.
        std::string message =
            refusalFor({{"Dip To Color Dissolve", 94, 106}}, items);
        Check(message.find("Dip To Color Dissolve") != std::string::npos &&
                  message.find("Cross Dissolve") != std::string::npos,
              "l'autre type est refusé sous son nom Resolve : " + message);

        // Une transition qui n'enjambe aucune coupe et qui n'est collée à
        // aucun bord de plan : ni fondu de raccord, ni fondu de bord.
        message = refusalFor({{"Cross Dissolve", 40, 60}}, items);
        Check(message.find("n'enjambe aucune coupe") != std::string::npos,
              "une transition au milieu d'un plan est refusée : " + message);

        // Réparti 8/4 autour de la coupe : Resolve garde ce partage (mesuré
        // par aller-retour), le document ne l'exprime pas.
        message = refusalFor({{"Cross Dissolve", 92, 104}}, items);
        Check(
            message.find("8 image(s) avant") != std::string::npos,
            "un partage que le document n'exprime pas est nommé : " + message);

        // Des poignées qui n'existent pas : le fondu lirait au-delà du rush.
        std::vector<TimelineItemFixture> tight;
        tight.push_back({"A", 0, 100, 100, 0, 0, "/rushes/R0.MP4", ""});
        tight.push_back({"B", 100, 200, 100, 0, 0, "/rushes/R1.MP4", ""});
        message = refusalFor({{"Cross Dissolve", 94, 106}}, tight);
        Check(message.find("oignées") != std::string::npos,
              "les poignées manquantes sont nommées : " + message);

        // Un fondu sur un plan ralenti : refusé par Document::Validate depuis
        // R3, nommé ici avant d'en arriver là.
        std::vector<TimelineItemFixture> slow = items;
        slow[0].left_offset = 80;
        slow[0].right_offset =
            620;  // 80 + 100 + 620 = 800 pour 400 à vitesse 1
        slow[0].media_frames = 800;
        message = refusalFor({{"Cross Dissolve", 94, 106}}, slow);
        Check(message.find("vitesse") != std::string::npos &&
                  message.find("R3") != std::string::npos,
              "un fondu sur un ralenti est refusé nommément : " + message);

        // Une transition sur une piste audio : le document n'en porte que sur
        // une piste vidéo.
        const std::string audioJson = R"({
            "schema": "cutmachine.resolve-timeline-read.v1",
            "project": "P", "name": "AUDIO", "frame_rate": 25,
            "origin": 90000, "width": 1080, "height": 1920,
            "tracks": [{"kind": "audio", "index": 1, "name": "A1",
              "clips": [
                {"name": "A", "record_start": 0, "record_end": 100,
                 "duration": 100, "source_start": 40, "path": "/rushes/R0.MP4",
                 "left_offset": 20, "right_offset": 130,
                 "media_frames": 500, "media_rate": 50},
                {"name": "B", "record_start": 100, "record_end": 200,
                 "duration": 100, "source_start": 40, "path": "/rushes/R1.MP4",
                 "left_offset": 20, "right_offset": 130,
                 "media_frames": 500, "media_rate": 50}],
              "transitions": [
                {"name": "Cross Dissolve", "record_start": 94,
                 "record_end": 106}]}]})";
        ResolveTimelineRead audio;
        ResolveTimelineImportPlan audioPlan;
        Check(ParseResolveTimelineManifest(audioJson, audio, error),
              "le manifeste audio se parse : " + error);
        Check(!PlanResolveTimelineImport(library, "/projet", audio, audioPlan,
                                         error),
              "une transition sur une piste audio est refusée");
        Check(error.find("piste audio") != std::string::npos,
              "la piste audio est nommée : " + error);

        CheckTimelineRejected(
            R"({"schema":"cutmachine.resolve-timeline-read.v1","name":"T",
                "frame_rate":25,"width":1080,"height":1920,"tracks":[
                {"kind":"video","index":1,"clips":[],"transitions":{}}]})",
            "'transitions' non tabulaire");
        CheckTimelineRejected(
            R"({"schema":"cutmachine.resolve-timeline-read.v1","name":"T",
                "frame_rate":25,"width":1080,"height":1920,"tracks":[
                {"kind":"video","index":1,"clips":[],"transitions":[
                 {"name":"Cross Dissolve","record_end":106}]}]})",
            "transition sans record_start");
    }

    // Critère 5 : l'aller-retour se ferme. Le document décrit un fondu, R9
    // l'écrit en <transitionitem>, Resolve le pose, la relecture le retrouve.
    // Les nombres de la relecture ne sont pas inventés : ce sont ceux que
    // Resolve 20.3.1 a rendus le 22 septembre sur les trois alignements,
    // mesurés par un vrai aller-retour (CM_FONDUS_RT, supprimée depuis).
    {
        const int32_t rate = 25;
        // La même bibliothèque des deux côtés : le retour doit retrouver les
        // ULID de média du départ, pas seulement des durées qui se
        // ressemblent.
        const Document emptyLibrary = SyntheticLibrary(2, 50, 2000);
        Document start = emptyLibrary;
        EditLog log;
        EditError editError = EditError::None;
        UpdateSequenceOperation sequence{start.sequence.id, "FONDU_AR", 1080,
                                         1920, MediaRate{rate, 1}};
        Check(log.Apply(start, sequence, editError, error),
              "la séquence de départ : " + error);
        AddTrackOperation track;
        track.track_id = GenerateUlid();
        track.kind = "video";
        track.index = 0;
        Check(log.Apply(start, track, editError, error),
              "la piste de départ : " + error);
        const Ulid leftId = GenerateUlid();
        const Ulid rightId = GenerateUlid();
        InsertClipOperation left;
        left.track_id = track.track_id;
        left.source_id = start.library[0].id;
        left.source_in = RationalTime{100, 50};
        left.duration = RationalTime{100, rate};
        left.timeline_in = RationalTime{0, rate};
        left.clip_id = leftId;
        Check(log.Apply(start, left, editError, error),
              "le plan de gauche : " + error);
        InsertClipOperation right;
        right.track_id = track.track_id;
        right.source_id = start.library[1].id;
        right.source_in = RationalTime{40, 50};
        right.duration = RationalTime{60, rate};
        right.timeline_in = RationalTime{100, rate};
        right.clip_id = rightId;
        Check(log.Apply(start, right, editError, error),
              "le plan de droite : " + error);

        for (const TransitionAlignment alignment :
             {TransitionAlignment::Center, TransitionAlignment::StartAtCut,
              TransitionAlignment::EndAtCut}) {
            Document document = start;
            EditLog trip;
            AddTransitionOperation add;
            add.transition.id = GenerateUlid();
            add.transition.track_id = track.track_id;
            add.transition.left_clip_id = leftId;
            add.transition.right_clip_id = rightId;
            add.transition.type = "cross_dissolve";
            add.transition.duration = RationalTime{12, rate};
            add.transition.alignment = alignment;
            Check(trip.Apply(document, add, editError, error),
                  "le fondu de départ : " + error);

            // Aller : R9 écrit le <transitionitem>.
            ResolveXmlTimeline xml;
            Check(BuildResolveXmlTimeline(document, xml, error),
                  "le document s'écrit en xmeml : " + error);
            Check(xml.transitions.size() == 1 && xml.clips.size() == 2,
                  "un fondu et deux plans partent");

            // Retour : ce que Resolve rend de ce xmeml. Mesuré : les bornes
            // du <transitionitem> reviennent inchangées, un plan garde sa
            // position, sa durée et son image source, et GetLeftOffset vaut
            // l'image source ramenée à la cadence de séquence.
            std::vector<TimelineItemFixture> items;
            for (const ResolveXmlClip& clip : xml.clips) {
                const ResolveXmlMedia& media = xml.media[clip.media];
                const int64_t span = clip.record_out - clip.record_in;
                const int64_t sourceStart =
                    clip.media_in * media.rate.num / rate;
                TimelineItemFixture item;
                item.name = media.filename;
                item.record_start = clip.record_in;
                item.record_end = clip.record_out;
                item.duration = span;
                item.source_start = sourceStart;
                item.path = media.path;
                item.media_frames = media.frames;
                item.media_rate = static_cast<int32_t>(media.rate.num);
                item.left_offset = clip.media_in;
                item.right_offset =
                    media.frames * rate / media.rate.num - clip.media_in - span;
                items.push_back(item);
            }
            std::vector<TimelineTransitionFixture> transitions;
            for (const ResolveXmlTransition& written : xml.transitions)
                transitions.push_back(
                    {"Cross Dissolve", written.start, written.end});

            ResolveTimelineRead read;
            ResolveTimelineImportPlan plan;
            Check(ParseResolveTimelineManifest(
                      TimelineManifest("FONDU_AR", rate, items, transitions),
                      read, error),
                  "le manifeste du retour se parse : " + error);
            Document back = emptyLibrary;
            Check(PlanResolveTimelineImport(back, "/projet", read, plan, error),
                  "le retour se planifie : " + error);
            EditLog backLog;
            UpdateSequenceOperation backSequence = plan.sequence;
            Check(backLog.Apply(back, backSequence, editError, error),
                  "la séquence du retour : " + error);
            for (AddTrackOperation created : plan.new_tracks)
                Check(backLog.Apply(back, created, editError, error),
                      "la piste du retour : " + error);
            for (InsertClipOperation insert : plan.clips)
                Check(backLog.Apply(back, insert, editError, error),
                      "le plan du retour : " + error);
            for (AddTransitionOperation applied : plan.transitions)
                Check(backLog.Apply(back, applied, editError, error),
                      "le fondu du retour : " + error);

            // Plan par plan.
            const DocumentTrack& before = document.sequence.tracks[0];
            const DocumentTrack& after = back.sequence.tracks[0];
            bool sameClips = before.clips.size() == after.clips.size();
            for (size_t index = 0; sameClips && index < before.clips.size();
                 ++index) {
                sameClips = before.clips[index].timeline_in ==
                                after.clips[index].timeline_in &&
                            before.clips[index].duration ==
                                after.clips[index].duration &&
                            before.clips[index].source_in ==
                                after.clips[index].source_in &&
                            before.clips[index].source_id ==
                                after.clips[index].source_id;
            }
            Check(sameClips,
                  "position, durée, plage source et média reviennent "
                  "à l'identique");
            // Transition par transition.
            Check(back.sequence.transitions.size() == 1,
                  "le fondu revient : " +
                      std::to_string(back.sequence.transitions.size()));
            const DocumentTransition& came = back.sequence.transitions[0];
            Check(came.type == add.transition.type &&
                      came.duration == add.transition.duration &&
                      came.alignment == add.transition.alignment,
                  "type, durée et alignement reviennent à l'identique");
            Check(came.left_clip_id == after.clips[0].id &&
                      came.right_clip_id == after.clips[1].id,
                  "le fondu raccorde les deux mêmes plans");
            Check(back.Validate(error),
                  "le document du retour valide : " + error);
        }
    }

    // Critère 3 : une présélection de 130 extraits pris sur 6 rushes se relit
    // sans doublon de média -- et ne peut pas en créer, R10 n'ingère rien.
    {
        const Document library = SyntheticLibrary(6, 25, 5000);
        std::vector<TimelineItemFixture> items;
        for (int index = 0; index < 130; ++index) {
            TimelineItemFixture item;
            item.name = "EXTRAIT_" + std::to_string(index);
            item.record_start = index * 30;
            item.record_end = index * 30 + 25;
            item.duration = 25;
            item.source_start = 40 + index * 3;
            item.path = "/rushes/R" + std::to_string(index % 6) + ".MP4";
            items.push_back(item);
        }
        ResolveTimelineRead read;
        ResolveTimelineImportPlan plan;
        Check(ParseResolveTimelineManifest(
                  TimelineManifest("PRESELECTION", 25, items), read, error),
              "le manifeste de présélection se parse : " + error);
        Check(PlanResolveTimelineImport(library, "/projet", read, plan, error),
              "la présélection se planifie : " + error);
        Check(plan.clips.size() == 130, "les 130 extraits sont planifiés : " +
                                            std::to_string(plan.clips.size()));
        std::set<Ulid> sources;
        for (const InsertClipOperation& insert : plan.clips)
            sources.insert(insert.source_id);
        Check(sources.size() == 6,
              "les extraits pointent les 6 rushes du Media Pool, sans "
              "doublon : " +
                  std::to_string(sources.size()));
        for (const Ulid& source : sources)
            Check(library.FindSource(source) != nullptr,
                  "chaque source planifiée est une source du document");
        Check(plan.retimed_clips == 0,
              "aucun des 130 extraits n'est ralenti, et le rapport le dit");
        Check(plan.clip_speeds.size() == plan.clips.size(),
              "une vitesse mesurée par plan planifié");

        // Appliqué puis renvoyé par le descripteur de R9 : positions, durées
        // et plages source reviennent à l'identique. C'est la moitié
        // vérifiable ici du critère 1 ; l'autre moitié demande Resolve.
        Document document = library;
        EditLog log;
        EditError editError = EditError::None;
        UpdateSequenceOperation sequence = plan.sequence;
        Check(log.Apply(document, sequence, editError, error),
              "la séquence prend le format de la timeline Resolve : " + error);
        for (AddTrackOperation track : plan.new_tracks)
            Check(log.Apply(document, track, editError, error),
                  "la piste se crée : " + error);
        for (InsertClipOperation insert : plan.clips)
            Check(log.Apply(document, insert, editError, error),
                  "le plan s'insère : " + error);
        Check(document.sequence.frame_rate.num == 25 &&
                  document.sequence.width == 1080 &&
                  document.sequence.height == 1920 &&
                  document.sequence.name == "PRESELECTION",
              "la séquence relue porte le nom et le format de Resolve");
        ResolveTimelineExport sent;
        Check(BuildResolveTimelineExport(document, sent, error),
              "le document relu se redécrit pour Resolve : " + error);
        Check(sent.clips.size() == 130, "130 plans repartent");
        bool identical = sent.clips.size() == items.size();
        for (size_t index = 0; identical && index < sent.clips.size();
             ++index) {
            identical =
                sent.clips[index].record_frame == items[index].record_start &&
                sent.clips[index].start_frame == items[index].source_start &&
                sent.clips[index].end_frame ==
                    items[index].source_start +
                        (items[index].record_end - items[index].record_start);
        }
        Check(identical,
              "chaque plan repart à sa position, sa durée et sa plage source");
    }

    // Un rush à 50 i/s dans une séquence à 25 : une image de séquence vaut
    // deux images source, et la conversion doit rester exacte.
    {
        Document library = SyntheticLibrary(1, 50, 1000);
        std::vector<TimelineItemFixture> items;
        items.push_back({"A", 0, 40, 80, 100, 0, "/rushes/R0.MP4", ""});
        ResolveTimelineRead read;
        ResolveTimelineImportPlan plan;
        Check(ParseResolveTimelineManifest(
                  TimelineManifest("CINQUANTE", 25, items), read, error),
              "manifeste 50 i/s : " + error);
        Check(PlanResolveTimelineImport(library, "/projet", read, plan, error),
              "un rush à 50 i/s dans une séquence à 25 se relit : " + error);
        Check(plan.clips.size() == 1 && plan.clips[0].source_in.value == 100 &&
                  plan.clips[0].source_in.rate == 50,
              "la plage source reste dans le domaine du rush");

        // Une durée qui ne tombe pas sur une image source entière se refuse.
        Document odd = SyntheticLibrary(1, 24, 1000);
        std::vector<TimelineItemFixture> inexact;
        inexact.push_back({"A", 0, 1, 1, 0, 0, "/rushes/R0.MP4", ""});
        ResolveTimelineRead second;
        ResolveTimelineImportPlan secondPlan;
        Check(ParseResolveTimelineManifest(
                  TimelineManifest("INEXACT", 25, inexact), second, error),
              "manifeste inexact : " + error);
        Check(!PlanResolveTimelineImport(odd, "/projet", second, secondPlan,
                                         error),
              "une image de séquence qui ne fait pas un nombre entier "
              "d'images source est refusée");
        Check(error.find("nombre entier") != std::string::npos,
              "la conversion inexacte est nommée : " + error);
    }

    // Une timeline qui porte déjà un montage n'en reçoit pas un second.
    {
        Document busy = SyntheticLibrary(1, 25, 500);
        busy.sequence.tracks.push_back({GenerateUlid(), "video", 0, {}});
        DocumentClip existing;
        existing.id = GenerateUlid();
        existing.source_id = busy.library[0].id;
        existing.source_in = {0, 25};
        existing.duration = {10, 25};
        existing.timeline_in = {0, 25};
        busy.sequence.tracks[0].clips.push_back(existing);
        std::vector<TimelineItemFixture> items;
        items.push_back({"A", 0, 40, 40, 0, 0, "/rushes/R0.MP4", ""});
        ResolveTimelineRead read;
        ResolveTimelineImportPlan plan;
        Check(ParseResolveTimelineManifest(TimelineManifest("SUR", 25, items),
                                           read, error),
              "manifeste sur timeline peuplée : " + error);
        Check(!PlanResolveTimelineImport(busy, "/projet", read, plan, error),
              "relire dans une timeline déjà montée est refusé");
        Check(error.find("porte déjà") != std::string::npos,
              "le refus dit ce que la timeline porte : " + error);
    }

    // --- R10 de bout en bout ----------------------------------------------
    // Le projet du test porte déjà les deux rushes ingérés plus haut : la
    // relecture ne fait que les pointer.
    {
        const std::filesystem::path refusedPath = root / "timeline-refus.json";
        {
            std::ofstream out(refusedPath);
            std::vector<TimelineItemFixture> items;
            items.push_back({"BON", 0, 4, 4, 0, 0, first.string(), ""});
            items.push_back(
                {"CADRE", 4, 8, 4, 0, 0, second.string(), "\"Pan\":\"100.0\""});
            out << TimelineManifest("RELUE", 25, items);
        }
        std::string refusedOutput;
        Check(ImportResolveTimelineCommand(projectPath, refusedPath.string(),
                                           refusedOutput) != 0,
              "une timeline non exprimable est refusée : " + refusedOutput);
        Check(refusedOutput.find("Pan=100.0") != std::string::npos &&
                  refusedOutput.find("\"refusals\"") != std::string::npos,
              "le refus nomme le plan et se lit machinalement : " +
                  refusedOutput);
        Project untouched;
        Check(LoadStoredProject(projectPath, untouched, error),
              "le projet se recharge après le refus : " + error);
        size_t clipsAfterRefusal = 0;
        for (const DocumentTrack& track :
             untouched.MakeActiveDocument().sequence.tracks)
            clipsAfterRefusal += track.clips.size();
        Check(clipsAfterRefusal == 0,
              "aucun document partiel n'est écrit quand un refus tombe");

        const std::filesystem::path acceptedPath = root / "timeline.json";
        {
            std::ofstream out(acceptedPath);
            std::vector<TimelineItemFixture> items;
            items.push_back({"P1", 0, 4, 4, 0, 0, first.string(), ""});
            items.push_back({"P2", 6, 10, 4, 2, 0, second.string(), ""});
            out << TimelineManifest("RELUE", 25, items);
        }
        std::string timelineOutput;
        Check(ImportResolveTimelineCommand(projectPath, acceptedPath.string(),
                                           timelineOutput) == 0,
              "la relecture réussit : " + timelineOutput);
        Check(
            timelineOutput.find("\"clips\":2") != std::string::npos &&
                timelineOutput.find("\"sources\":2") != std::string::npos &&
                timelineOutput.find("\"retimed_clips\":0") != std::string::npos,
            "le rapport compte plans, sources et plans ralentis : " +
                timelineOutput);
        Project relu;
        Check(LoadStoredProject(projectPath, relu, error),
              "le projet se recharge après la relecture : " + error);
        const Document reread = relu.MakeActiveDocument();
        Check(reread.sequence.name == "RELUE" && reread.library.size() == 2,
              "la séquence prend le nom de la timeline et aucun média n'est "
              "ajouté");
        size_t clips = 0;
        for (const DocumentTrack& track : reread.sequence.tracks)
            clips += track.clips.size();
        Check(clips == 2, "les deux plans sont dans le document : " +
                              std::to_string(clips));

        // PRD17 -- ROADMAP.md. Relue dans la timeline nommée, et non dans
        // l'active : un agent d'une série relit dans son film.
        AddProjectTimelineOperation added;
        added.name = "Série B";
        added.width = 1920;
        added.height = 1080;
        added.frame_rate = {25, 1};
        added.timeline_id = "01K36000000000000000000077";
        std::string addOutput;
        Check(
            ApplyProjectOperationCommand(
                projectPath, SerializeProjectOperation(added), addOutput) == 0,
            "une seconde timeline s'ajoute : " + addOutput);
        std::string namedOutput;
        Check(ImportResolveTimelineCommand(projectPath, acceptedPath.string(),
                                           namedOutput, added.timeline_id) == 0,
              "la relecture vise la timeline nommée : " + namedOutput);
        Project named;
        Check(LoadStoredProject(projectPath, named, error),
              "le projet se recharge : " + error);
        size_t namedClips = 0;
        for (const DocumentTrack& track :
             named.MakeDocument(added.timeline_id).sequence.tracks)
            namedClips += track.clips.size();
        Check(namedClips == 2 &&
                  named.active_timeline_id == relu.active_timeline_id &&
                  named.MakeActiveDocument().SaveToString() ==
                      reread.SaveToString(),
              "la timeline nommée reçoit les plans, l'active ne bouge pas");
        std::string unknownOutput;
        Check(
            ImportResolveTimelineCommand(projectPath, acceptedPath.string(),
                                         unknownOutput,
                                         "01K3600000000000000000ZZZZ") != 0 &&
                unknownOutput.find("UnknownSequence") != std::string::npos,
            "une timeline inconnue est refusée par son nom : " + unknownOutput);
    }

    // R10 -- un fondu enchaîné traverse la commande entière, dans un second
    // paquet : relire dans une timeline déjà montée est refusé, donc le
    // premier ne peut pas resservir.
    {
        std::string fondusPath;
        Check(
            CreatePortableProject((root / "Fondus.cutmachine-project").string(),
                                  Project("Fondus"), fondusPath, error),
            "le second paquet projet se crée : " + error);
        Check(ImportResolveCommand(fondusPath, manifestPath.string(), output) ==
                  0,
              "les rushes entrent dans le second paquet : " + output);
        const std::filesystem::path fondusManifest = root / "fondus.json";
        {
            std::ofstream out(fondusManifest);
            std::vector<TimelineItemFixture> items;
            items.push_back({"P1", 0, 4, 4, 0, 0, first.string(), ""});
            items.push_back({"P2", 4, 8, 4, 4, 0, second.string(), ""});
            // Comme Resolve les pose : à cheval sur la coupe, 1 image de
            // chaque côté, en chevauchant les deux plans.
            out << TimelineManifest("FONDUE", 25, items,
                                    {{"Cross Dissolve", 3, 5}});
        }
        std::string fondusOutput;
        Check(ImportResolveTimelineCommand(fondusPath, fondusManifest.string(),
                                           fondusOutput) == 0,
              "la relecture avec fondu réussit : " + fondusOutput);
        Check(fondusOutput.find("\"clips\":2") != std::string::npos &&
                  fondusOutput.find("\"transitions\":1") != std::string::npos,
              "le rapport compte le fondu : " + fondusOutput);
        Project withFondu;
        Check(LoadStoredProject(fondusPath, withFondu, error),
              "le second projet se recharge : " + error);
        const Document reread = withFondu.MakeActiveDocument();
        Check(reread.sequence.transitions.size() == 1 &&
                  reread.sequence.transitions[0].type == "cross_dissolve" &&
                  reread.sequence.transitions[0].duration ==
                      (RationalTime{2, 25}) &&
                  reread.sequence.transitions[0].alignment ==
                      TransitionAlignment::Center,
              "le fondu est dans le document, centré, de 2 images");
        Check(reread.Validate(error),
              "le document relu avec son fondu valide : " + error);
        Check(fondusOutput.find("\"grades\"") == std::string::npos,
              "sans base Resolve, la réponse est celle d'avant DEC15");
    }

    // DEC15 (GR15) -- docs/roadmap/GRADE-RESOLVE-2026-10.md. L'étalonnage
    // de Resolve, lu dans une copie de sa base, revient avec la relecture.
    {
        std::string etaloPath;
        Check(
            CreatePortableProject((root / "Etalo.cutmachine-project").string(),
                                  Project("Etalo"), etaloPath, error),
            "le troisième paquet projet se crée : " + error);
        Check(
            ImportResolveCommand(etaloPath, manifestPath.string(), output) == 0,
            "les rushes entrent dans le troisième paquet : " + output);
        const std::filesystem::path etaloManifest = root / "etalo.json";
        {
            std::ofstream out(etaloManifest);
            std::vector<TimelineItemFixture> items;
            items.push_back({"C8015.MP4", 0, 4, 4, 0, 0, first.string(), ""});
            items.push_back({"C8035.MP4", 4, 8, 4, 4, 0, second.string(), ""});
            out << TimelineManifest("ETALONNEE", 25, items);
        }
        const auto bodies = resolve_fixture::LoadBodies(
            std::filesystem::path(RESOLVE_GRADE_FIXTURES_DIR) / "bodies.txt");
        resolve_fixture::Timeline timeline;
        timeline.name = "ETALONNEE";
        resolve_fixture::Item graded;
        graded.start = 90000;
        graded.duration = 4;
        graded.name = "C8015.MP4";
        graded.path = first.string();
        graded.body = bodies.at("offset_window");
        resolve_fixture::Item plain = graded;
        plain.start = 90004;
        plain.name = "C8035.MP4";
        plain.path = second.string();
        plain.correction = false;
        plain.body = bodies.at("quiet_flags");
        timeline.items = {graded, plain};
        const std::filesystem::path database = root / "Project.db";
        Check(resolve_fixture::Write(database, {timeline}, error),
              "la base Resolve de test s'écrit : " + error);
        resolve_fixture::Timeline elsewhere = timeline;
        elsewhere.name = "AUTRE";
        const std::filesystem::path otherDatabase = root / "Autre.db";
        Check(resolve_fixture::Write(otherDatabase, {elsewhere}, error),
              "la seconde base de test s'écrit : " + error);
        const std::filesystem::path notDatabase = root / "Texte.db";
        std::ofstream(notDatabase) << "pas une base";

        const auto clipsIn = [&](const std::string& path) {
            Project project;
            Check(LoadStoredProject(path, project, error),
                  "le projet se recharge : " + error);
            size_t count = 0;
            for (const DocumentTrack& track :
                 project.MakeActiveDocument().sequence.tracks)
                count += track.clips.size();
            return count;
        };
        std::string refused;
        Check(ImportResolveTimelineCommand(etaloPath, etaloManifest.string(),
                                           refused, "",
                                           notDatabase.string()) != 0 &&
                  refused.find("ParseError") != std::string::npos,
              "une base illisible refuse la relecture : " + refused);
        Check(clipsIn(etaloPath) == 0,
              "rien n'est écrit quand la base ne se lit pas");
        refused.clear();
        Check(ImportResolveTimelineCommand(etaloPath, etaloManifest.string(),
                                           refused, "",
                                           otherDatabase.string()) != 0 &&
                  refused.find("absente de la base") != std::string::npos,
              "une base sans la timeline refuse la relecture : " + refused);
        Check(clipsIn(etaloPath) == 0,
              "rien n'est écrit quand la timeline manque à la base");

        std::string etaloOutput;
        Check(ImportResolveTimelineCommand(etaloPath, etaloManifest.string(),
                                           etaloOutput, "",
                                           database.string()) == 0,
              "la relecture avec la base réussit : " + etaloOutput);
        Project etalo;
        Check(LoadStoredProject(etaloPath, etalo, error),
              "le projet se recharge après la relecture : " + error);
        Ulid gradedId;
        for (const DocumentTrack& track :
             etalo.MakeActiveDocument().sequence.tracks)
            for (const DocumentClip& clip : track.clips)
                if (clip.timeline_in == RationalTime{0, 25}) gradedId = clip.id;
        Check(!gradedId.empty() &&
                  etaloOutput.find("\"grades\":{") != std::string::npos &&
                  etaloOutput.find("\"clip_id\":\"" + gradedId + "\"") !=
                      std::string::npos &&
                  etaloOutput.find("\"offset.r\":-0.25") != std::string::npos &&
                  etaloOutput.find("fenêtre (correcteur 4)") !=
                      std::string::npos &&
                  etaloOutput.find("\"ungraded\":1") != std::string::npos,
              "la réponse porte l'étalonnage du plan, par son id : " +
                  etaloOutput);
        Check(etalo.MakeActiveDocument().SaveToString().find(
                  "color.resolve_primaries") == std::string::npos &&
                  etaloOutput.find("\"written\":0") != std::string::npos &&
                  etaloOutput.find("que la lecture nomme sans le lire") !=
                      std::string::npos,
              "GR16 : un étalonnage sous une fenêtre ne s'écrit pas, et la "
              "réponse dit pourquoi : " +
                  etaloOutput);
    }

    // GR16 -- docs/roadmap/GRADE-RESOLVE-2026-10.md. Un étalonnage que
    // color.resolve_primaries rend entièrement entre dans le document, dans
    // le journal de la relecture.
    {
        std::string renduPath;
        Check(
            CreatePortableProject((root / "Rendu.cutmachine-project").string(),
                                  Project("Rendu"), renduPath, error),
            "le paquet du rendu se crée : " + error);
        Check(
            ImportResolveCommand(renduPath, manifestPath.string(), output) == 0,
            "les rushes entrent dans le paquet du rendu : " + output);
        const std::filesystem::path renduManifest = root / "rendu.json";
        {
            std::ofstream out(renduManifest);
            std::vector<TimelineItemFixture> items;
            items.push_back({"C8015.MP4", 0, 4, 4, 0, 0, first.string(), ""});
            out << TimelineManifest("RENDU", 25, items);
        }
        const auto bodies = resolve_fixture::LoadBodies(
            std::filesystem::path(RESOLVE_GRADE_FIXTURES_DIR) / "bodies.txt");
        resolve_fixture::Timeline timeline;
        timeline.name = "RENDU";
        resolve_fixture::Item graded;
        graded.start = 90000;
        graded.duration = 4;
        graded.name = "C8015.MP4";
        graded.path = first.string();
        graded.body = bodies.at("rendered_primaries");
        timeline.items = {graded};
        const std::filesystem::path database = root / "Rendu.db";
        // GR16 -- a project in DaVinci YRGB: the saturation of the second
        // node renders only with the colour science read.
        Check(
            resolve_fixture::Write(database, {timeline}, error) &&
                resolve_fixture::WriteConfig(
                    database, resolve_fixture::ConfigBlob(false, true), error),
            "la base du rendu s'écrit : " + error);
        std::string renduOutput;
        Check(ImportResolveTimelineCommand(renduPath, renduManifest.string(),
                                           renduOutput, "",
                                           database.string()) == 0,
              "la relecture du rendu réussit : " + renduOutput);
        Project rendu;
        Check(LoadStoredProject(renduPath, rendu, error),
              "le projet du rendu se recharge : " + error);
        const Document document = rendu.MakeActiveDocument();
        std::vector<std::string> types;
        for (const DocumentTrack& track : document.sequence.tracks)
            for (const DocumentClip& clip : track.clips)
                if (clip.timeline_in == RationalTime{0, 25})
                    for (const ClipEffect& effect : clip.effects)
                        if (!effect.id.empty()) types.push_back(effect.type);
        Check(types == std::vector<std::string>{"color.resolve_primaries",
                                                "color.resolve_primaries"} &&
                  renduOutput.find("\"written\":1") != std::string::npos &&
                  renduOutput.find("\"written_stages\":2") != std::string::npos,
              "les deux nœuds entrent sur le plan, chacun avec son id, et la "
              "réponse le dit : " +
                  renduOutput);
        Check(document.SaveToString().find("\"version\": 22") !=
                  std::string::npos,
              "le document passe en version 22");
    }

    // GR16 -- le contraste s'écrit quand les réglages du projet se lisent
    // dans sa configuration (SM_Config), et pas sans eux.
    {
        const auto bodies = resolve_fixture::LoadBodies(
            std::filesystem::path(RESOLVE_GRADE_FIXTURES_DIR) / "bodies.txt");
        const auto relire = [&](const std::string& tag, bool withConfig,
                                std::string& answer, int science = -1,
                                bool aware = true) {
            std::string packagePath;
            Check(CreatePortableProject(
                      (root / (tag + ".cutmachine-project")).string(),
                      Project(tag), packagePath, error),
                  "le paquet " + tag + " se crée : " + error);
            Check(ImportResolveCommand(packagePath, manifestPath.string(),
                                       output) == 0,
                  "les rushes entrent dans " + tag + " : " + output);
            const std::filesystem::path manifest = root / (tag + ".json");
            {
                std::ofstream out(manifest);
                std::vector<TimelineItemFixture> items;
                items.push_back(
                    {"C8015.MP4", 0, 4, 4, 0, 0, first.string(), ""});
                out << TimelineManifest("CONTRASTE", 25, items);
            }
            resolve_fixture::Timeline timeline;
            timeline.name = "CONTRASTE";
            resolve_fixture::Item graded;
            graded.start = 90000;
            graded.duration = 4;
            graded.name = "C8015.MP4";
            graded.path = first.string();
            graded.body = bodies.at("contrasted_primaries");
            timeline.items = {graded};
            const std::filesystem::path database = root / (tag + ".db");
            Check(resolve_fixture::Write(database, {timeline}, error) &&
                      (!withConfig || resolve_fixture::WriteConfig(
                                          database,
                                          resolve_fixture::ConfigBlob(
                                              aware, true, -1, science),
                                          error)),
                  "la base " + tag + " s'écrit : " + error);
            Check(ImportResolveTimelineCommand(packagePath, manifest.string(),
                                               answer, "",
                                               database.string()) == 0,
                  "la relecture " + tag + " réussit : " + answer);
            Project project;
            Check(LoadStoredProject(packagePath, project, error),
                  "le projet " + tag + " se recharge : " + error);
            std::vector<ClipEffect> effects;
            for (const DocumentTrack& track :
                 project.MakeActiveDocument().sequence.tracks)
                for (const DocumentClip& clip : track.clips)
                    if (clip.timeline_in == RationalTime{0, 25})
                        effects = clip.effects;
            return effects;
        };
        std::string answer;
        const std::vector<ClipEffect> written =
            relire("Contraste", true, answer);
        const auto param = [&](const char* name, int32_t num, int32_t den) {
            if (written.empty()) return false;
            const auto found = written[0].params.find(name);
            return found != written[0].params.end() &&
                   found->second.num == num && found->second.den == den;
        };
        Check(written.size() == 1 && param("contrast", 5, 4) &&
                  param("pivot", 13, 32) && param("color_space_aware", 1, 1) &&
                  param("contrast_s_curve", 1, 1) && param("gain_r", 9, 8) &&
                  answer.find("\"project_settings\":{\"read\":true,"
                              "\"color_space_aware\":true") !=
                      std::string::npos &&
                  answer.find("\"working_luminance\":\"HDR 1000\"") !=
                      std::string::npos &&
                  answer.find("\"color_science\":\"DaVinci YRGB\"") !=
                      std::string::npos,
              "le contraste, son pivot et les réglages du projet entrent sur "
              "le plan, et la réponse dit les réglages lus : " +
                  answer);
        // GR16 -- the same grade in a project in ACEScct (field 7 at 8), with
        // the aware tools off as every ACEScct project of the library: the
        // contrast enters, the stage marked acescct (14 November 2026).
        const std::vector<ClipEffect> aces =
            relire("ACEScct", true, answer, 8, false);
        const auto acesParam = [&](const char* name, int32_t num) {
            if (aces.empty()) return false;
            const auto found = aces[0].params.find(name);
            return found != aces[0].params.end() && found->second.num == num &&
                   found->second.den == 1;
        };
        Check(aces.size() == 1 && acesParam("acescct", 1) &&
                  acesParam("color_space_aware", 0) &&
                  answer.find("\"color_science\":\"ACEScct\"") !=
                      std::string::npos,
              "en ACEScct, le contraste s'écrit, l'étape marquée acescct, et "
              "la réponse nomme la science couleur : " +
                  answer);
        // With the aware tools on, never measured in ACEScct: not written.
        const std::vector<ClipEffect> acesAware =
            relire("ACEScctAware", true, answer, 8);
        Check(acesAware.empty() &&
                  answer.find("\"written\":0") != std::string::npos &&
                  answer.find("mesuré en ACEScct outils") != std::string::npos,
              "en ACEScct outils « color space aware » allumés, le contraste "
              "ne s'écrit pas, et la réponse dit pourquoi : " +
                  answer);
        const std::vector<ClipEffect> without =
            relire("SansReglages", false, answer);
        Check(without.empty() &&
                  answer.find("\"written\":0") != std::string::npos &&
                  answer.find("contrast 1.25") != std::string::npos,
              "sans configuration du projet, le contraste ne s'écrit pas, et "
              "la réponse dit pourquoi : " +
                  answer);
    }

    std::error_code cleanup;
    std::filesystem::remove_all(root, cleanup);
    if (failures == 0) std::cout << "resolve import tests passed\n";
    return failures == 0 ? 0 : 1;
}
