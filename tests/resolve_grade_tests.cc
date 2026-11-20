// DEC15 (GR15) -- docs/roadmap/GRADE-RESOLVE-2026-10.md. What was graded in
// a Resolve project, read from a copy of its database: the bodies
// (tests/fixtures/resolve_grade/bodies.txt, written field by field by
// make_bodies.py), the database (a reduced one written here,
// resolve_database_fixture.h), and the match onto the clips an import plans.
//
//   cutmachine_resolve_grade_tests --read <Project.db> [<timeline>]
//
// reads a real database instead -- a copy, as the engine does -- and prints
// what the reading finds, every timeline when none is named: how the
// reading was checked against the projects on the Mac.
//
//   cutmachine_resolve_grade_tests --compare <Project.db> <folder>
//
// GR16 -- holds what the import writes against Resolve itself, on real
// grades: <folder> holds cubes Resolve's ExportLUT gave for clips of that
// project, and an index.json naming each one's clip ({"items": [{"track",
// "clip", "start", "cube"}]}). Each clip is found in the database, its
// grade translated as the import translates it, and every lattice point of
// the cube put through the stages, in the node's own values as ExportLUT
// samples them.

#include "ColorEffects.h"
#include "Json.h"
#include "ResolveGrade.h"
#include "ResolveImport.h"
#include "Ulid.h"

#include "resolve_database_fixture.h"

#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "TestSupport.h"

#ifndef RESOLVE_GRADE_FIXTURES_DIR
#error "RESOLVE_GRADE_FIXTURES_DIR must be defined"
#endif

namespace {

const ResolveGradeSetting* FindSetting(const ResolveGradeNode& node,
                                       const std::string& name) {
    for (const ResolveGradeSetting& setting : node.settings)
        if (setting.name == name) return &setting;
    return nullptr;
}

bool HasUnread(const ResolveGrade& grade, int64_t node,
               const std::string& fragment) {
    for (const ResolveGradeUnread& unread : grade.unread)
        if (unread.node == node &&
            unread.what.find(fragment) != std::string::npos)
            return true;
    return false;
}

std::string Describe(const ResolveGrade& grade) {
    return SerializeResolveGrade(grade);
}

// A timeline manifest: video items on V1 and V2, an audio item on A1, at
// speed 1 over 25 i/s media of 500 frames.
struct ManifestItem {
    std::string kind = "video";
    int index = 1;
    std::string name;
    int64_t record_start = 0;
    int64_t record_end = 0;
    int64_t source_start = 0;
    std::string path;
};

std::string Manifest(const std::string& name,
                     const std::vector<ManifestItem>& items) {
    std::map<std::pair<std::string, int>, std::vector<const ManifestItem*>>
        tracks;
    for (const ManifestItem& item : items)
        tracks[{item.kind, item.index}].push_back(&item);
    std::ostringstream json;
    json << R"({"schema":"cutmachine.resolve-timeline-read.v1",)"
         << R"("project":"P","name":")" << name
         << R"(","frame_rate":25,"origin":90000,"width":1920,"height":1080,)"
         << R"("unpublished":[],"tracks":[)";
    bool firstTrack = true;
    for (const auto& track : tracks) {
        if (!firstTrack) json << ',';
        firstTrack = false;
        json << R"({"kind":")" << track.first.first << R"(","index":)"
             << track.first.second << R"(,"name":"","clips":[)";
        for (size_t index = 0; index < track.second.size(); ++index) {
            const ManifestItem& item = *track.second[index];
            const int64_t span = item.record_end - item.record_start;
            if (index) json << ',';
            json << R"({"name":")" << item.name << R"(","record_start":)"
                 << item.record_start << R"(,"record_end":)" << item.record_end
                 << R"(,"duration":)" << span << R"(,"source_start":)"
                 << item.source_start << R"(,"fusion_comps":0,"path":")"
                 << item.path << R"(","left_offset":)" << item.source_start
                 << R"(,"right_offset":)" << (500 - item.source_start - span)
                 << R"(,"media_frames":500,"media_rate":25})";
        }
        json << R"(],"transitions":[]})";
    }
    json << "]}";
    return json.str();
}

Document Library(const std::vector<std::string>& paths) {
    Document document;
    for (const std::string& path : paths) {
        LibraryMedia media;
        media.id = GenerateUlid();
        media.path = path;
        media.filename = std::filesystem::path(path).filename().string();
        media.codec = "h264";
        media.width = 1920;
        media.height = 1080;
        media.orientation = "landscape";
        media.rate = {25, 1};
        media.duration = {500, 25};
        document.library.push_back(media);
        document.sources.push_back(
            {media.id, media.path, media.rate, media.duration});
    }
    return document;
}

size_t CopiesLeft(const std::filesystem::path& directory) {
    size_t count = 0;
    std::error_code ignored;
    for (const auto& entry :
         std::filesystem::directory_iterator(directory, ignored))
        if (entry.path().filename().string().rfind("cutmachine-resolve-", 0) ==
            0)
            ++count;
    return count;
}

std::string FileBytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream bytes;
    bytes << input.rdbuf();
    return bytes.str();
}

// --read: every timeline named, or all of them.
int ReadRealDatabase(const std::string& path, const std::string& only) {
    std::vector<std::string> names;
    if (!only.empty()) {
        names.push_back(only);
    } else {
        // The names are listed from the database itself, read-only; the
        // reading proper goes through its own copy.
        sqlite3* db = nullptr;
        if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) !=
            SQLITE_OK) {
            std::cerr << "cannot open " << path << '\n';
            sqlite3_close(db);
            return 1;
        }
        sqlite3_stmt* statement = nullptr;
        sqlite3_prepare_v2(db, "select distinct Name from Sm2Timeline", -1,
                           &statement, nullptr);
        while (statement != nullptr && sqlite3_step(statement) == SQLITE_ROW)
            names.emplace_back(reinterpret_cast<const char*>(
                sqlite3_column_text(statement, 0)));
        sqlite3_finalize(statement);
        sqlite3_close(db);
    }
    size_t clips = 0, graded = 0, unreadable = 0, settings = 0;
    std::map<std::string, size_t> unread;
    // GR16 -- what the import would write: grades rendered whole, and the
    // first reason of the others, by the setting or the thing it names.
    size_t written = 0;
    std::map<std::string, size_t> unrendered;
    for (const std::string& name : names) {
        std::vector<ResolveDatabaseTimeline> timelines;
        std::string error;
        ResolveProjectSettings project;
        if (!ReadResolveDatabaseTimelines(path, name, timelines, error,
                                          &project)) {
            std::cout << "{\"timeline\":\"" << name << "\",\"error\":\""
                      << error << "\"}\n";
            continue;
        }
        for (const ResolveDatabaseTimeline& timeline : timelines) {
            for (const ResolveDatabaseClip& clip : timeline.clips) {
                ++clips;
                if (!clip.graded) continue;
                ++graded;
                for (const ResolveGradeNode& node : clip.grade.nodes)
                    settings += node.settings.size();
                for (const ResolveGradeUnread& entry : clip.grade.unread) {
                    if (entry.what.rfind("étalonnage illisible", 0) == 0)
                        ++unreadable;
                    std::string family = entry.what;
                    const size_t cut = family.find(" « ");
                    if (cut != std::string::npos) family.resize(cut);
                    ++unread[family];
                }
                std::vector<ClipEffect> stages;
                std::vector<std::string> reasons;
                ResolveGradeStages(clip.grade, stages, reasons, project);
                if (!stages.empty()) ++written;
                for (const std::string& reason : reasons) {
                    std::string family = reason.substr(reason.find(" : ") + 3);
                    family.resize(std::min(family.find(' '), family.size()));
                    ++unrendered[family];
                }
                if (!only.empty())
                    std::cout << "{\"track\":" << clip.track
                              << ",\"start\":" << clip.start
                              << ",\"duration\":" << clip.duration
                              << ",\"clip\":\"" << clip.name
                              << "\",\"grade\":" << Describe(clip.grade)
                              << "}\n";
            }
            if (timeline.graded && !only.empty())
                std::cout << "{\"timeline_grade\":" << Describe(timeline.grade)
                          << "}\n";
        }
    }
    std::cout << "{\"timelines\":" << names.size() << ",\"clips\":" << clips
              << ",\"graded\":" << graded << ",\"unreadable\":" << unreadable
              << ",\"settings\":" << settings << ",\"unread\":{";
    bool first = true;
    for (const auto& entry : unread) {
        if (!first) std::cout << ',';
        first = false;
        std::cout << '"' << entry.first << "\":" << entry.second;
    }
    std::cout << "},\"written\":" << written << ",\"unrendered\":{";
    first = true;
    for (const auto& entry : unrendered) {
        if (!first) std::cout << ',';
        first = false;
        std::cout << '"' << entry.first << "\":" << entry.second;
    }
    std::cout << "}}\n";
    return 0;
}

// --compare: a cube as Resolve writes it, red fastest; empty when the file
// is not one.
std::vector<std::array<float, 3>> ReadCube(const std::filesystem::path& path,
                                           int32_t& size) {
    std::ifstream input(path);
    std::vector<std::array<float, 3>> rows;
    size = 0;
    std::string line;
    while (std::getline(input, line)) {
        std::istringstream fields(line);
        std::string first;
        if (!(fields >> first)) continue;
        if (first == "LUT_3D_SIZE") {
            fields >> size;
        } else {
            char* end = nullptr;
            std::array<float, 3> row{};
            row[0] = std::strtof(first.c_str(), &end);
            if (end != first.c_str() && *end == '\0' &&
                (fields >> row[1] >> row[2]))
                rows.push_back(row);
        }
    }
    if (size < 2 || rows.size() != static_cast<size_t>(size) * size * size)
        rows.clear();
    return rows;
}

int CompareWithResolveCubes(const std::string& path,
                            const std::filesystem::path& folder) {
    mcp_json::Value index;
    std::string error;
    if (!mcp_json::Value::Parse(FileBytes(folder / "index.json"), index,
                                error) ||
        index.Find("items") == nullptr) {
        std::cerr << "index.json: " << error << '\n';
        return 1;
    }
    // Every timeline of the database, read once through the engine's copy.
    std::vector<std::string> names;
    {
        sqlite3* db = nullptr;
        if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) !=
            SQLITE_OK) {
            std::cerr << "cannot open " << path << '\n';
            sqlite3_close(db);
            return 1;
        }
        sqlite3_stmt* statement = nullptr;
        sqlite3_prepare_v2(db, "select distinct Name from Sm2Timeline", -1,
                           &statement, nullptr);
        while (statement != nullptr && sqlite3_step(statement) == SQLITE_ROW)
            names.emplace_back(reinterpret_cast<const char*>(
                sqlite3_column_text(statement, 0)));
        sqlite3_finalize(statement);
        sqlite3_close(db);
    }
    std::vector<ResolveDatabaseTimeline> timelines;
    ResolveProjectSettings project;
    for (const std::string& name : names) {
        std::vector<ResolveDatabaseTimeline> named;
        if (ReadResolveDatabaseTimelines(path, name, named, error, &project))
            timelines.insert(timelines.end(), named.begin(), named.end());
    }
    // The timeline the cubes came from: the one holding the most of them.
    const auto& items = index.Find("items")->AsArray();
    const auto found = [](const ResolveDatabaseTimeline& timeline,
                          const mcp_json::Value& item) {
        int64_t track = 0;
        int64_t start = 0;
        item.Find("track")->AsInt64(track);
        item.Find("start")->AsInt64(start);
        for (const ResolveDatabaseClip& clip : timeline.clips) {
            if (clip.track == track && clip.start == start &&
                clip.name == item.Find("clip")->AsString())
                return &clip;
        }
        return static_cast<const ResolveDatabaseClip*>(nullptr);
    };
    const ResolveDatabaseTimeline* source = nullptr;
    size_t best = 0;
    for (const ResolveDatabaseTimeline& timeline : timelines) {
        size_t held = 0;
        for (const mcp_json::Value& item : items)
            if (found(timeline, item) != nullptr) ++held;
        if (held > best) {
            best = held;
            source = &timeline;
        }
    }
    if (source == nullptr) {
        std::cerr << "no timeline holds the cubes' clips\n";
        return 1;
    }
    std::cout << "{\"timeline\":\"" << source->name << "\",\"found\":" << best
              << ",\"cubes\":" << items.size()
              << ",\"settings\":{\"read\":" << (project.read ? "true" : "false")
              << ",\"color_space_aware\":"
              << (project.color_space_aware ? "true" : "false")
              << ",\"contrast_s_curve\":"
              << (project.contrast_s_curve ? "true" : "false") << "}}\n";
    size_t compared = 0;
    double worstOfAll = 0.0;
    for (const mcp_json::Value& item : items) {
        const std::string cubeName = item.Find("cube")->AsString();
        std::cout << "{\"cube\":\"" << cubeName << '"';
        const ResolveDatabaseClip* clip = found(*source, item);
        int32_t size = 0;
        const std::vector<std::array<float, 3>> cube =
            ReadCube(folder / cubeName, size);
        if (clip == nullptr || cube.empty()) {
            std::cout << ",\"error\":\""
                      << (clip == nullptr ? "clip not in the timeline"
                                          : "not a cube")
                      << "\"}\n";
            continue;
        }
        std::vector<ClipEffect> stages;
        std::vector<std::string> reasons;
        ResolveGradeStages(clip->grade, stages, reasons, project);
        // A grade that writes nothing and says nothing is neutral: the clip
        // gets no stage, so Resolve's cube must be the identity.
        if (stages.empty() && !reasons.empty()) {
            std::cout << ",\"written\":false,\"unrendered\":[";
            for (size_t reason = 0; reason < reasons.size(); ++reason)
                std::cout << (reason != 0 ? "," : "") << '"' << reasons[reason]
                          << '"';
            std::cout << "]}\n";
            continue;
        }
        // In the node's own values: the law with no colour management
        // around it, as ExportLUT samples a node.
        const ResolvedColorGrade grade = ResolveColorGrade(stages, {});
        std::vector<double> errors;
        errors.reserve(cube.size() * 3);
        double worst = 0.0;
        double greyWorst = 0.0;
        std::array<float, 3> worstAt{};
        for (size_t point = 0; point < cube.size(); ++point) {
            const auto lattice = static_cast<size_t>(size);
            const std::array<size_t, 3> at = {point % lattice,
                                              (point / lattice) % lattice,
                                              point / (lattice * lattice)};
            const auto steps = static_cast<float>(size - 1);
            const std::array<float, 3> in = {static_cast<float>(at[0]) / steps,
                                             static_cast<float>(at[1]) / steps,
                                             static_cast<float>(at[2]) / steps};
            const std::array<float, 3> out = ApplyResolvedColorGrade(in, grade);
            for (size_t channel = 0; channel < 3; ++channel) {
                const double error =
                    std::fabs(std::clamp(out[channel], 0.0f, 1.0f) -
                              cube[point][channel]) *
                    1023.0;
                errors.push_back(error);
                if (error > worst) {
                    worst = error;
                    worstAt = in;
                }
                if (in[0] == in[1] && in[1] == in[2])
                    greyWorst = std::max(greyWorst, error);
            }
        }
        std::sort(errors.begin(), errors.end());
        double mean = 0.0;
        for (double error : errors) mean += error;
        mean /= static_cast<double>(errors.size());
        ++compared;
        worstOfAll = std::max(worstOfAll, worst);
        std::cout << ",\"written\":" << (stages.empty() ? "false" : "true")
                  << ",\"stages\":" << stages.size()
                  << ",\"codes\":{\"worst\":" << worst
                  << ",\"p99\":" << errors[(errors.size() * 99) / 100]
                  << ",\"p50\":" << errors[errors.size() / 2]
                  << ",\"mean\":" << mean << ",\"grey_worst\":" << greyWorst
                  << "},\"worst_at\":[" << worstAt[0] << ',' << worstAt[1]
                  << ',' << worstAt[2] << "],\"params\":{";
        bool first = true;
        for (const ClipEffect& stage : stages) {
            for (const auto& [name, value] : stage.params) {
                if (!first) std::cout << ',';
                first = false;
                std::cout << '"' << name << "\":\"" << value.num << '/'
                          << value.den << '"';
            }
        }
        std::cout << "}}\n";
    }
    std::cout << "{\"compared\":" << compared << ",\"worst\":" << worstOfAll
              << "}\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 3 && std::string(argv[1]) == "--read")
        return ReadRealDatabase(argv[2], argc >= 4 ? argv[3] : "");
    if (argc >= 4 && std::string(argv[1]) == "--compare")
        return CompareWithResolveCubes(argv[2], argv[3]);

    const auto bodies = resolve_fixture::LoadBodies(
        std::filesystem::path(RESOLVE_GRADE_FIXTURES_DIR) / "bodies.txt");
    Check(bodies.size() >= 11, "les corps de test se lisent");
    const auto body = [&](const std::string& name) {
        const auto found = bodies.find(name);
        Check(found != bodies.end(), "corps de test présent : " + name);
        return found != bodies.end() ? found->second : std::string();
    };
    std::string error;

    // --- Les corps ----------------------------------------------------------
    {
        ResolveGrade grade;
        Check(DecodeResolveGradeBody(body("offset_window"), grade, error),
              "un offset sous une fenêtre se lit : " + error);
        Check(grade.nodes.size() == 1 && grade.serial,
              "un nœud : " + Describe(grade));
        if (!grade.nodes.empty()) {
            const ResolveGradeNode& node = grade.nodes[0];
            const ResolveGradeSetting* red = FindSetting(node, "offset.r");
            const ResolveGradeSetting* green = FindSetting(node, "offset.g");
            const ResolveGradeSetting* blue = FindSetting(node, "offset.b");
            Check(node.number == 1 && node.enabled && red && green && blue &&
                      red->value == -0.25f && green->value == 0.5f &&
                      blue->value == 0.125f && node.settings.size() == 3,
                  "les trois offsets sont lus tels que stockés : " +
                      Describe(grade));
        }
        Check(HasUnread(grade, 1, "fenêtre (correcteur 4)"),
              "la fenêtre est nommée, non lue : " + Describe(grade));
        Check(grade.unread.size() == 1,
              "le qualificateur au repos ne dit rien : " + Describe(grade));
    }
    {
        // Deux nœuds en chaîne, rangés à l'envers dans le corps.
        for (const std::string& name :
             {std::string("serial_primaries"),
              std::string("serial_primaries_raw"),
              std::string("serial_primaries_stream")}) {
            ResolveGrade grade;
            Check(DecodeResolveGradeBody(body(name), grade, error),
                  name + " se lit : " + error);
            Check(
                grade.serial && grade.nodes.size() == 2 && grade.unread.empty(),
                name + " : deux nœuds en série, rien à taire : " +
                    Describe(grade));
            if (grade.nodes.size() != 2) continue;
            Check(grade.nodes[0].number == 1 && grade.nodes[1].number == 2,
                  name + " : dans l'ordre de la chaîne : " + Describe(grade));
            const ResolveGradeSetting* contrast =
                FindSetting(grade.nodes[0], "contrast");
            const ResolveGradeSetting* temperature =
                FindSetting(grade.nodes[0], "temperature");
            const ResolveGradeSetting* gamma =
                FindSetting(grade.nodes[1], "gamma.master");
            Check(
                contrast && contrast->value == 1.25f && temperature &&
                    temperature->value == -80.0f && gamma &&
                    gamma->value == 0.5f &&
                    FindSetting(grade.nodes[0], "pivot") &&
                    FindSetting(grade.nodes[0], "tint") &&
                    FindSetting(grade.nodes[0], "saturation") &&
                    grade.nodes[1].settings.size() == 5,
                name + " : les primaires de chaque nœud : " + Describe(grade));
            Check(grade.nodes[1].label == "Peau",
                  name + " : le nom du nœud : " + Describe(grade));
        }
    }
    {
        ResolveGrade grade;
        Check(DecodeResolveGradeBody(body("plugin_curve_lut"), grade, error),
              "greffon, courbe et LUT se lisent : " + error);
        Check(grade.nodes.size() == 1 && grade.nodes[0].settings.size() == 1 &&
                  FindSetting(grade.nodes[0], "contrast"),
              "seul le contraste est une valeur lue : " + Describe(grade));
        Check(HasUnread(grade, 1,
                        "greffon OFX « com.blackmagicdesign.resolvefx."
                        "vignette »") &&
                  HasUnread(grade, 1, "courbes HSL") &&
                  HasUnread(grade, 1, "LUT « Fixture/Film to Video.cube »") &&
                  HasUnread(grade, 1, "paramètre 0x860000B4 inconnu") &&
                  grade.unread.size() == 4,
              "chacun nommé, une fois : " + Describe(grade));
    }
    {
        ResolveGrade grade;
        Check(DecodeResolveGradeBody(body("quiet_flags"), grade, error) &&
                  grade.unread.empty() && grade.nodes.size() == 1 &&
                  grade.nodes[0].settings.size() == 1,
              "des courbes HSL laissées plates ne disent rien : " +
                  Describe(grade) + error);
    }
    {
        ResolveGrade grade;
        Check(DecodeResolveGradeBody(body("keyframed"), grade, error),
              "des primaires animées se lisent : " + error);
        Check(grade.nodes.size() == 1 && grade.nodes[0].settings.empty() &&
                  HasUnread(grade, 1, "primaires : animé, 2 images clés"),
              "animées, elles sont nommées et non lues : " + Describe(grade));
    }
    {
        ResolveGrade grade;
        Check(DecodeResolveGradeBody(body("parallel"), grade, error),
              "des nœuds parallèles se lisent : " + error);
        Check(
            !grade.serial && grade.nodes.size() == 3 &&
                HasUnread(grade, 0, "câblage des 3 nœuds"),
            "le câblage est nommé, l'ordre n'est pas lu : " + Describe(grade));
    }
    {
        ResolveGrade grade;
        Check(DecodeResolveGradeBody(body("outside_setting"), grade, error),
              "un réglage hors des nœuds se lit : " + error);
        Check(HasUnread(grade, 0, "hors des nœuds : réglage (correcteur 10)"),
              "il est nommé : " + Describe(grade));
    }
    {
        ResolveGrade grade;
        Check(DecodeResolveGradeBody(body("disabled_compound"), grade, error),
              "nœuds désactivé et composé se lisent : " + error);
        Check(grade.nodes.size() == 2 && !grade.nodes[0].enabled &&
                  grade.nodes[0].settings.size() == 1 &&
                  grade.nodes[1].enabled && grade.nodes[1].settings.empty() &&
                  HasUnread(grade, 2, "primaires : correcteur désactivé") &&
                  HasUnread(grade, 2, "nœud composé"),
              "l'état d'un nœud absent se lit éteint ; un correcteur éteint "
              "et un nœud composé sont nommés : " +
                  Describe(grade));
    }
    {
        // Ce qui ne se lit pas du tout est un refus, pas un étalonnage vide.
        ResolveGrade grade;
        Check(
            !DecodeResolveGradeBody(std::string("\x7f\x01", 2), grade, error) &&
                error.find("0x7F") != std::string::npos,
            "un en-tête inconnu est refusé par son nom : " + error);
        std::string truncated = body("offset_window");
        truncated.resize(truncated.size() / 2);
        error.clear();
        Check(!DecodeResolveGradeBody(truncated, grade, error) &&
                  error.find("zstd") != std::string::npos,
              "une trame tronquée est refusée : " + error);
        error.clear();
        Check(!DecodeResolveGradeBody(std::string("\x80\x0a\x7f\x01", 4), grade,
                                      error) &&
                  error.find("protobuf") != std::string::npos,
              "un protobuf faux est refusé : " + error);
        error.clear();
        Check(!DecodeResolveGradeBody(std::string(), grade, error),
              "un corps vide est refusé");
        const std::string json = SerializeResolveGrade(grade);
        Check(json.find("\"nodes\":[]") != std::string::npos,
              "un refus ne laisse aucun nœud : " + json);
    }

    // --- La base -------------------------------------------------------------
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("cutmachine-resolve-grade-tests-" + GenerateUlid());
    std::filesystem::create_directories(root / "tmp");
    // The engine copies into the temporary directory: a private one, so
    // that the copies left behind are counted without another suite's.
    ::setenv("TMPDIR", (root / "tmp").string().c_str(), 1);
    const std::filesystem::path database = root / "Project.db";
    {
        using resolve_fixture::Item;
        using resolve_fixture::Timeline;
        std::vector<Timeline> timelines;
        Timeline film;
        film.name = "Film";
        film.body = body("timeline_saturation");
        Item a;
        a.track = 1;
        a.start = 90000;
        a.duration = 25;
        a.name = "A.MP4";
        a.path = "/rushes/A.MP4";
        a.body = body("offset_window");
        Item b = a;
        b.start = 90025;
        b.duration = 50;
        b.name = "B.MP4";
        b.path = "/rushes/B.MP4";
        b.body = body("serial_primaries");
        b.version = "Version 2";
        Item c = a;
        c.start = 90075;
        c.duration = 25;
        c.name = "C.MP4";
        c.path = "/rushes/C.MP4";
        c.body = body("quiet_flags");
        c.correction = false;
        Item d = a;
        d.track = 2;
        d.start = 90010;
        d.duration = 10;
        d.name = "D.MP4";
        d.path = "/rushes/D.MP4";
        d.body = body("quiet_flags");
        d.correction = true;
        d.remote = true;
        d.remote_correction = true;
        Item e = a;
        e.track = 2;
        e.start = 90050;
        e.duration = 10;
        e.name = "E.MP4";
        e.path = "/rushes/E.MP4";
        e.body = body("quiet_flags");
        e.group = "Interview";
        e.group_correction = true;
        film.items = {a, b, c, d, e};
        timelines.push_back(film);
        // Two timelines of one name: one carries the cut, the other not.
        Timeline twinA;
        twinA.name = "Double";
        twinA.items = {a};
        Timeline twinB = twinA;
        Item moved = a;
        moved.start = 90100;
        twinB.items = {moved};
        twinB.items[0].body = body("keyframed");
        timelines.push_back(twinA);
        timelines.push_back(twinB);
        Timeline same1;
        same1.name = "Pareil";
        same1.items = {a};
        timelines.push_back(same1);
        timelines.push_back(same1);
        Check(resolve_fixture::Write(database, timelines, error),
              "la base de test s'écrit : " + error);
    }
    const std::string before = FileBytes(database);
    {
        std::vector<ResolveDatabaseTimeline> read;
        Check(ReadResolveDatabaseTimelines(database.string(), "Film", read,
                                           error),
              "la base se lit : " + error);
        Check(read.size() == 1 && read[0].clips.size() == 5,
              "une timeline, cinq plans vidéo");
        if (read.size() == 1 && read[0].clips.size() == 5) {
            const ResolveDatabaseTimeline& film = read[0];
            Check(
                film.graded && film.grade.nodes.size() == 1 &&
                    FindSetting(film.grade.nodes[0], "saturation"),
                "l'étalonnage de la timeline est lu : " + Describe(film.grade));
            std::map<std::string, const ResolveDatabaseClip*> byName;
            for (const ResolveDatabaseClip& clip : film.clips)
                byName[clip.name] = &clip;
            const ResolveDatabaseClip* a = byName["A.MP4"];
            const ResolveDatabaseClip* b = byName["B.MP4"];
            const ResolveDatabaseClip* c = byName["C.MP4"];
            const ResolveDatabaseClip* d = byName["D.MP4"];
            const ResolveDatabaseClip* e = byName["E.MP4"];
            Check(a && a->track == 1 && a->start == 90000 &&
                      a->duration == 25 && a->whole_frames &&
                      a->path == "/rushes/A.MP4" && a->graded &&
                      HasUnread(a->grade, 1, "fenêtre"),
                  "le plan A, à sa place, avec sa fenêtre nommée");
            Check(b && b->graded && b->grade.version == "Version 2" &&
                      b->grade.nodes.size() == 2,
                  "le plan B porte sa version active");
            Check(c && !c->graded, "un plan sans correction n'a rien à dire");
            Check(d && d->track == 2 && d->graded &&
                      HasUnread(d->grade, 0, "version distante (remote)"),
                  "une version distante étalonnée est nommée");
            Check(e && e->graded &&
                      HasUnread(e->grade, 0, "groupe « Interview » étalonné"),
                  "un groupe étalonné est nommé");
        }
        Check(FileBytes(database) == before,
              "la base n'est pas touchée : seule sa copie est lue");
        Check(CopiesLeft(root / "tmp") == 0,
              "la copie est effacée après la lecture");
    }
    {
        std::vector<ResolveDatabaseTimeline> read;
        Check(ReadResolveDatabaseTimelines(database.string(), "Absente", read,
                                           error) &&
                  read.empty(),
              "une timeline absente n'est pas une erreur de lecture");
        Check(!ReadResolveDatabaseTimelines((root / "rien.db").string(), "Film",
                                            read, error) &&
                  error.find("introuvable") != std::string::npos,
              "une base absente est refusée par son nom : " + error);
        const std::filesystem::path text = root / "texte.db";
        std::ofstream(text) << "ceci n'est pas une base";
        error.clear();
        Check(
            !ReadResolveDatabaseTimelines(text.string(), "Film", read, error) &&
                !error.empty(),
            "un fichier qui n'est pas une base est refusé : " + error);
        const std::filesystem::path other = root / "autre.db";
        {
            sqlite3* db = nullptr;
            sqlite3_open(other.string().c_str(), &db);
            sqlite3_exec(db, "create table Sm2Timeline (Name text)", nullptr,
                         nullptr, nullptr);
            sqlite3_close(db);
        }
        error.clear();
        Check(!ReadResolveDatabaseTimelines(other.string(), "Film", read,
                                            error) &&
                  error.find("non reconnue") != std::string::npos,
              "un autre schéma est refusé par son nom : " + error);
        Check(CopiesLeft(root / "tmp") == 0,
              "aucune copie ne reste, même après un refus");
    }

    // --- Sur les plans d'un import -------------------------------------------
    {
        const Document document = Library(
            {"/rushes/A.MP4", "/rushes/B.MP4", "/rushes/C.MP4", "/rushes/D.MP4",
             "/rushes/E.MP4", "/rushes/F.MP4", "/rushes/SON.WAV"});
        std::vector<ManifestItem> items = {
            {"video", 1, "A.MP4", 0, 25, 0, "/rushes/A.MP4"},
            {"video", 1, "B.MP4", 25, 75, 10, "/rushes/B.MP4"},
            {"video", 1, "C.MP4", 75, 100, 0, "/rushes/C.MP4"},
            {"video", 2, "D.MP4", 10, 20, 0, "/rushes/D.MP4"},
            {"video", 2, "E.MP4", 50, 60, 0, "/rushes/E.MP4"},
            // Not in the database: the copy predates it.
            {"video", 2, "F.MP4", 70, 80, 0, "/rushes/F.MP4"},
            {"audio", 1, "SON.WAV", 0, 100, 0, "/rushes/SON.WAV"},
        };
        ResolveTimelineRead manifest;
        Check(ParseResolveTimelineManifest(Manifest("Film", items), manifest,
                                           error),
              "le manifeste se lit : " + error);
        ResolveTimelineImportPlan plan;
        Check(PlanResolveTimelineImport(document, "/projet", manifest, plan,
                                        error),
              "l'import se planifie : " + error);
        Check(plan.clip_origins.size() == plan.clips.size(),
              "chaque plan sait d'où il vient");
        std::vector<ResolveDatabaseTimeline> timelines;
        Check(ReadResolveDatabaseTimelines(database.string(), "Film", timelines,
                                           error),
              "la base se relit : " + error);
        ResolveGradeReading reading;
        reading.database = database.string();
        Check(MatchResolveTimelineGrades(manifest, plan, timelines, reading,
                                         error),
              "les étalonnages s'apparient : " + error);
        std::map<std::string, Ulid> idByName;
        for (size_t index = 0; index < plan.clips.size(); ++index) {
            const auto origin = plan.clip_origins[index];
            idByName[manifest.tracks[origin.track].clips[origin.item].name] =
                plan.clips[index].clip_id;
        }
        Check(reading.clips.size() == 4 && reading.ungraded == 1 &&
                  reading.unmatched.size() == 1 && reading.timeline_graded,
              "quatre plans étalonnés, un sans, un sans place, la timeline : " +
                  SerializeResolveGradeReading(reading));
        std::set<std::string> graded;
        for (const ResolveClipGrade& clip : reading.clips) {
            graded.insert(clip.clip);
            Check(clip.clip_id == idByName[clip.clip],
                  "le plan " + clip.clip +
                      " porte l'id que le plan d'import "
                      "lui donne");
        }
        Check(
            graded == std::set<std::string>{"A.MP4", "B.MP4", "D.MP4", "E.MP4"},
            "les plans étalonnés sont les bons");
        if (reading.unmatched.size() == 1)
            Check(reading.unmatched[0].clip == "F.MP4" &&
                      reading.unmatched[0].track == "V2" &&
                      reading.unmatched[0].reason.find("aucun plan") !=
                          std::string::npos,
                  "le plan absent de la base est nommé : " +
                      reading.unmatched[0].reason);
        const std::string json = SerializeResolveGradeReading(reading);
        Check(json.find("\"offset.r\":-0.25") != std::string::npos &&
                  json.find("\"timeline_grade\":{") != std::string::npos &&
                  json.find("\"note\":\"valeurs telles que Resolve les "
                            "stocke") != std::string::npos,
              "la lecture se sérialise : " + json);

        // Le même plan, à la même place, mais un autre fichier.
        std::vector<ManifestItem> relinked = {
            {"video", 1, "A.MP4", 0, 25, 0, "/rushes/B.MP4"}};
        ResolveTimelineRead other;
        ResolveTimelineImportPlan otherPlan;
        Check(ParseResolveTimelineManifest(Manifest("Film", relinked), other,
                                           error) &&
                  PlanResolveTimelineImport(document, "/projet", other,
                                            otherPlan, error),
              "un second import se planifie : " + error);
        ResolveGradeReading otherReading;
        Check(MatchResolveTimelineGrades(other, otherPlan, timelines,
                                         otherReading, error) &&
                  otherReading.unmatched.size() == 1 &&
                  otherReading.unmatched[0].reason.find(
                      "lit « /rushes/A.MP4 »") != std::string::npos,
              "un autre fichier à la même place n'est pas apparié");
    }
    {
        // Deux timelines du même nom : celle qui porte tout le montage.
        const Document document = Library({"/rushes/A.MP4"});
        std::vector<ManifestItem> items = {
            {"video", 1, "A.MP4", 0, 25, 0, "/rushes/A.MP4"}};
        for (const std::string& name :
             {std::string("Double"), std::string("Pareil"),
              std::string("Absente")}) {
            ResolveTimelineRead manifest;
            ResolveTimelineImportPlan plan;
            Check(ParseResolveTimelineManifest(Manifest(name, items), manifest,
                                               error) &&
                      PlanResolveTimelineImport(document, "/projet", manifest,
                                                plan, error),
                  name + " se planifie : " + error);
            std::vector<ResolveDatabaseTimeline> timelines;
            Check(ReadResolveDatabaseTimelines(database.string(), name,
                                               timelines, error),
                  name + " se lit : " + error);
            ResolveGradeReading reading;
            error.clear();
            const bool matched = MatchResolveTimelineGrades(
                manifest, plan, timelines, reading, error);
            if (name == "Double") {
                Check(matched && reading.clips.size() == 1 &&
                          reading.unmatched.empty() &&
                          HasUnread(reading.clips[0].grade, 1, "fenêtre"),
                      "des deux « Double », celle qui porte le montage : " +
                          error);
            } else if (name == "Pareil") {
                Check(!matched && error.find("2 timelines « Pareil »") !=
                                      std::string::npos,
                      "deux timelines qui portent le montage : refusé par "
                      "son nom : " +
                          error);
            } else {
                Check(!matched &&
                          error.find("absente de la base") != std::string::npos,
                      "une timeline absente de la base est refusée : " + error);
            }
        }
    }

    // --- GR16 : la lecture en étapes color.resolve_primaries ---------------
    {
        const auto fraction = [](const ClipEffect& stage, const char* name,
                                 int32_t num, int32_t den) {
            const auto found = stage.params.find(name);
            return found != stage.params.end() && found->second.num == num &&
                   found->second.den == den;
        };
        const auto joined = [](const std::vector<std::string>& lines) {
            std::string all;
            for (const std::string& line : lines) all += line + " | ";
            return all;
        };
        // A project in DaVinci YRGB, its configuration read (the default
        // colour science, field 7 absent).
        ResolveProjectSettings yrgb;
        yrgb.read = true;
        yrgb.unread.clear();
        ResolveGrade grade;
        std::vector<ClipEffect> stages;
        std::vector<std::string> unrendered;
        Check(DecodeResolveGradeBody(body("rendered_primaries"), grade, error),
              "deux nœuds de primaires se lisent : " + error);
        ResolveGradeStages(grade, stages, unrendered, yrgb);
        Check(unrendered.empty() && stages.size() == 2,
              "deux nœuds que l'étape rend donnent deux étapes : " +
                  joined(unrendered));
        if (stages.size() == 2) {
            Check(stages[0].type == "color.resolve_primaries" &&
                      stages[0].id.empty() &&
                      fraction(stages[0], "gain_r", 5, 4) &&
                      fraction(stages[0], "gain_master", 5, 4) &&
                      fraction(stages[0], "gamma_r", 1, 8) &&
                      fraction(stages[0], "lift_master", -1, 16) &&
                      fraction(stages[0], "offset_r", -1, 4) &&
                      stages[0].params.size() == 7,
                  "le premier nœud, ses valeurs stockées en fractions "
                  "exactes, sans id (l'opération en donne un)");
            Check(fraction(stages[1], "saturation", 9, 8) &&
                      fraction(stages[1], "lum_mix", 1, 2) &&
                      stages[1].params.size() == 2,
                  "le second nœud vient après, dans l'ordre de la chaîne");
        }
        // GR16 -- the saturation moves by up to 18 codes in ACEScct: without
        // the project's colour science, it is not written.
        ResolveGradeStages(grade, stages, unrendered);
        Check(stages.empty() && unrendered.size() == 1 &&
                  unrendered[0].find("le nœud 2 : saturation 1.125, "
                                     "réglages du projet non lus") !=
                      std::string::npos,
              "sans réglages lus, la saturation ne s'écrit pas, et la raison "
              "le dit : " +
                  joined(unrendered));
        grade = {};
        Check(DecodeResolveGradeBody(body("serial_primaries"), grade, error),
              "les primaires en chaîne se lisent : " + error);
        ResolveGradeStages(grade, stages, unrendered);
        std::string reasons = joined(unrendered);
        Check(stages.empty() && unrendered.size() == 4 &&
                  reasons.find("le nœud 1 : contrast 1.25") !=
                      std::string::npos &&
                  reasons.find("le nœud 1 : temperature -80") !=
                      std::string::npos &&
                  reasons.find("le nœud 1 : tint -6.5") != std::string::npos &&
                  reasons.find("le nœud 1 : saturation 1.125") !=
                      std::string::npos,
              "sans les réglages du projet, rien n'est écrit, et le "
              "contraste, la saturation, la température et la teinte sont "
              "nommés : " +
                  reasons);
        // GR16 -- in DaVinci YRGB and in Color Managed v2, the node's
        // temperature and tint enter its stage as stored (the cubes of both
        // are the same to the last digit, 6 November 2026).
        ResolveProjectSettings managed = yrgb;
        managed.color_science = "DaVinci YRGB Color Managed";
        for (const ResolveProjectSettings& read : {yrgb, managed}) {
            ResolveGradeStages(grade, stages, unrendered, read);
            Check(unrendered.empty() && stages.size() == 2 &&
                      fraction(stages[0], "temperature", -80, 1) &&
                      fraction(stages[0], "tint", -13, 2) &&
                      fraction(stages[0], "contrast", 5, 4) &&
                      fraction(stages[0], "saturation", 9, 8),
                  "en " + read.color_science +
                      ", la température −80 et la teinte −6,5 s'écrivent sur "
                      "l'étape du premier nœud : " +
                      joined(unrendered));
        }
        // GR16 -- in ACEScct (14 November 2026) the same node is written
        // too, every stage carrying acescct: its laws there are the stage's.
        ResolveProjectSettings aces = yrgb;
        aces.color_science = "ACEScct";
        ResolveGradeStages(grade, stages, unrendered, aces);
        Check(unrendered.empty() && stages.size() == 2 &&
                  fraction(stages[0], "temperature", -80, 1) &&
                  fraction(stages[0], "tint", -13, 2) &&
                  fraction(stages[0], "contrast", 5, 4) &&
                  fraction(stages[0], "color_space_aware", 0, 1) &&
                  fraction(stages[0], "acescct", 1, 1) &&
                  fraction(stages[1], "acescct", 1, 1),
              "en ACEScct, le contraste, la température, la teinte et la "
              "saturation s'écrivent, chaque étape marquée acescct : " +
                  joined(unrendered));
        // With the aware tools, never in an ACEScct project of the library,
        // its contrast follows another law: refused, the reason says why.
        ResolveProjectSettings acesAware = aces;
        acesAware.color_space_aware = true;
        ResolveGradeStages(grade, stages, unrendered, acesAware);
        reasons = joined(unrendered);
        Check(stages.empty() && unrendered.size() == 1 &&
                  reasons.find("le nœud 1 : contrast 1.25, mesuré en ACEScct "
                               "outils « color space aware » coupés "
                               "seulement") != std::string::npos,
              "en ACEScct outils allumés, le contraste ne s'écrit pas, et la "
              "raison le dit : " +
                  reasons);
        {
            // The lift, gamma, gain, offset and saturation render in ACEScct
            // (the cubes of 6 November 2026), the stage carrying acescct;
            // nothing does in a science never measured.
            ResolveGrade gains;
            gains.nodes.push_back({1,
                                   "",
                                   true,
                                   {{"gain.r", 1.25f},
                                    {"lift.master", -0.0625f},
                                    {"saturation", 1.5f}}});
            ResolveGradeStages(gains, stages, unrendered, aces);
            Check(unrendered.empty() && stages.size() == 1 &&
                      fraction(stages[0], "acescct", 1, 1) &&
                      fraction(stages[0], "saturation", 3, 2),
                  "en ACEScct, un lift, un gain et une saturation s'écrivent, "
                  "l'étape marquée acescct : " +
                      joined(unrendered));
            ResolveGradeStages(gains, stages, unrendered, yrgb);
            Check(stages.size() == 1 && stages[0].params.count("acescct") == 0,
                  "en DaVinci YRGB, l'étape ne porte pas acescct");
            ResolveProjectSettings acescc = yrgb;
            acescc.color_science = "ACEScc";
            ResolveGradeStages(gains, stages, unrendered, acescc);
            Check(stages.empty() &&
                      joined(unrendered)
                              .find("le nœud 1 : en ACEScc, où "
                                    "color.resolve_primaries n'a "
                                    "jamais été mesurée") != std::string::npos,
                  "en ACEScc, jamais mesuré, rien ne s'écrit : " +
                      joined(unrendered));
        }
        grade = {};
        Check(DecodeResolveGradeBody(body("offset_window"), grade, error),
              "l'offset sous une fenêtre se relit : " + error);
        ResolveGradeStages(grade, stages, unrendered);
        Check(stages.empty() &&
                  joined(unrendered).find("fenêtre") != std::string::npos,
              "ce que la lecture nomme sans le lire empêche d'écrire : " +
                  joined(unrendered));

        ResolveGrade typed;
        typed.nodes.push_back({1,
                               "",
                               true,
                               {{"gain.r", 1.106f},
                                {"lift.g", 1e-9f},
                                {"offset.b", -0.0058384049f},
                                {"contrast", 1.0f},
                                {"pivot", 0.6f},
                                {"temperature", 0.0f},
                                {"log.low_range", 0.333f}}});
        typed.nodes.push_back({2, "", false, {{"gamma.b", 0.5f}}});
        typed.nodes.push_back({3, "", true, {{"gain.r", 1.0f}}});
        ResolveGradeStages(typed, stages, unrendered);
        Check(unrendered.empty() && stages.size() == 1 &&
                  fraction(stages[0], "gain_r", 553, 500) &&
                  fraction(stages[0], "lift_g", 0, 1) &&
                  fraction(stages[0], "offset_b", -1167681, 200000000),
              "1,106 stocké s'écrit 553/500, un reste de 1e-9 vaut 0, un "
              "offset de dix décimales se prend au milliardième ; un "
              "contraste neutre, un pivot et une plage Log sans effet, un "
              "nœud éteint et un nœud neutre ne disent rien : " +
                  joined(unrendered));
        typed.nodes.push_back({4, "", true, {{"gamma.r", 4.5f}}});
        ResolveGradeStages(typed, stages, unrendered);
        Check(stages.empty() && joined(unrendered).find("outside [-4, 4]") !=
                                    std::string::npos,
              "une valeur hors des bornes de l'étape empêche d'écrire : " +
                  joined(unrendered));
    }

    // --- GR16 : les réglages du projet, et le contraste ---------------------
    {
        ResolveProjectSettings settings = ParseResolveProjectSettings(
            resolve_fixture::ConfigBlob(true, false));
        Check(settings.read && settings.color_space_aware &&
                  !settings.contrast_s_curve && settings.unread.empty(),
              "les outils « color space aware » allumés et la courbe en S "
              "coupée se lisent : " +
                  settings.unread);
        settings = ParseResolveProjectSettings(
            resolve_fixture::ConfigBlob(false, true));
        Check(settings.read && !settings.color_space_aware &&
                  settings.contrast_s_curve,
              "absents, les outils sont coupés et la courbe en S allumée, "
              "comme dans Resolve");
        Check(settings.working_luminance == "HDR 1000",
              "absente, la luminance de travail est HDR 1000, comme dans "
              "Resolve");
        const std::vector<std::pair<int, const char*>> luminances = {
            {0, "SDR 100"}, {6, "HDR 2000"}, {8, "HDR 4000"}, {3, "valeur 3"}};
        for (const auto& [code, name] : luminances) {
            settings = ParseResolveProjectSettings(
                resolve_fixture::ConfigBlob(true, true, code));
            Check(settings.read && settings.color_space_aware &&
                      settings.working_luminance == name,
                  std::string("le champ 202 à ") + std::to_string(code) +
                      " se lit " + name + " : " + settings.working_luminance);
        }
        // GR16 -- field 7, the colour science (6 November 2026).
        Check(settings.color_science == "DaVinci YRGB",
              "absente, la science couleur est DaVinci YRGB, comme dans "
              "Resolve");
        const std::vector<std::pair<int, const char*>> sciences = {
            {2, "DaVinci YRGB Color Managed v1"},
            {6, "ACEScc"},
            {8, "ACEScct"},
            {10, "DaVinci YRGB Color Managed"},
            {4, "valeur 4"}};
        for (const auto& [code, name] : sciences) {
            settings = ParseResolveProjectSettings(
                resolve_fixture::ConfigBlob(false, true, -1, code));
            Check(settings.read && settings.color_science == name &&
                      settings.working_luminance == "HDR 1000",
                  std::string("le champ 7 à ") + std::to_string(code) +
                      " se lit " + name + " : " + settings.color_science);
        }
        Check(ResolveScienceRendersBalance("DaVinci YRGB") &&
                  ResolveScienceRendersBalance("DaVinci YRGB Color Managed") &&
                  ResolveScienceRendersBalance("ACEScct") &&
                  ResolveScienceRendersPrimaries("ACEScct") &&
                  !ResolveScienceRendersBalance("ACEScc") &&
                  !ResolveScienceRendersPrimaries("ACEScc") &&
                  !ResolveScienceRendersPrimaries(
                      "DaVinci YRGB Color Managed v1") &&
                  !ResolveScienceRendersPrimaries("valeur 4"),
              "les sciences mesurées : YRGB, Color Managed v2 et ACEScct, "
              "rien ailleurs");
        settings = ParseResolveProjectSettings("pas une configuration");
        Check(!settings.read && !settings.unread.empty(),
              "une configuration illisible laisse les réglages non lus : " +
                  settings.unread);

        ResolveProjectSettings aware;
        aware.read = true;
        aware.color_space_aware = true;
        aware.unread.clear();
        ResolveGrade contrasted;
        contrasted.nodes.push_back(
            {1,
             "",
             true,
             {{"contrast", 1.2f}, {"pivot", 0.4f}, {"gain.r", 1.1f}}});
        std::vector<ClipEffect> stages;
        std::vector<std::string> unrendered;
        ResolveGradeStages(contrasted, stages, unrendered, aware);
        const auto has = [&](const char* name, int32_t num, int32_t den) {
            if (stages.empty()) return false;
            const auto found = stages[0].params.find(name);
            return found != stages[0].params.end() &&
                   found->second.num == num && found->second.den == den;
        };
        Check(unrendered.empty() && stages.size() == 1 &&
                  has("contrast", 6, 5) && has("pivot", 2, 5) &&
                  has("color_space_aware", 1, 1) &&
                  has("contrast_s_curve", 1, 1) && has("gain_r", 11, 10),
              "un contraste et son pivot s'écrivent avec les réglages du "
              "projet");
        ResolveGradeStages(contrasted, stages, unrendered);
        Check(stages.empty() && !unrendered.empty() &&
                  unrendered[0].find("contrast 1.2") != std::string::npos,
              "sans réglages lus, le contraste ne s'écrit pas, et la raison "
              "le dit");
        // GR16 -- the aware contrast was measured at HDR 1000 only; without
        // the aware tools the working luminance changes nothing.
        ResolveProjectSettings sdr = aware;
        sdr.working_luminance = "SDR 100";
        ResolveGradeStages(contrasted, stages, unrendered, sdr);
        Check(stages.empty() && unrendered.size() == 1 &&
                  unrendered[0].find("pas en SDR 100") != std::string::npos,
              "outils allumés en SDR 100, le contraste ne s'écrit pas, et la "
              "raison nomme la luminance");
        sdr.color_space_aware = false;
        ResolveGradeStages(contrasted, stages, unrendered, sdr);
        Check(unrendered.empty() && stages.size() == 1 &&
                  has("contrast", 6, 5) && has("color_space_aware", 0, 1),
              "outils coupés, le contraste s'écrit quelle que soit la "
              "luminance de travail");
        ResolveGrade plain;
        plain.nodes.push_back({1, "", true, {{"gain.r", 1.1f}}});
        sdr.color_space_aware = true;
        ResolveGradeStages(plain, stages, unrendered, sdr);
        Check(unrendered.empty() && stages.size() == 1,
              "sans contraste, la luminance de travail ne bloque rien");
        contrasted.nodes[0].settings[0].value = 1.9f;
        ResolveGradeStages(contrasted, stages, unrendered, aware);
        Check(stages.empty() && !unrendered.empty() &&
                  unrendered[0].find("9/5") != std::string::npos,
              "un contraste au-delà de la table ne s'écrit pas");
    }

    std::error_code cleanup;
    std::filesystem::remove_all(root, cleanup);
    if (failures == 0) std::cout << "resolve grade tests passed\n";
    return failures == 0 ? 0 : 1;
}
