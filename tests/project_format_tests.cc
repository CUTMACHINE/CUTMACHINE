// S7 -- SAVING_ROADMAP.md. Project file version 3 stores the library, bins
// and sources once beside the timelines, where version 2 embedded one whole
// document -- library included -- per timeline.
//
// Version 3 must round-trip canonically, every version-2 file must keep
// loading into the same project, and the file must stop growing with the
// number of timelines times the size of the library.

#include "DocumentJson.h"
#include "Project.h"
#include "ProjectFixture.h"
#include "ProjectVersionTwo.h"

#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>

namespace {

void Check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

using project_version_two::Escaped;
using project_version_two::VersionTwo;

}  // namespace

int main() {
    std::string error;
    Project project = MakeInterviewShapedProject();
    project.name = "Crépuscule \"ITW\"";
    project.settings.color_management.enabled = true;
    project.settings.color_management.input_gamut = "sony_sgamut3_cine";
    project.settings.color_management.input_transfer = "sony_slog3";
    project.rushes[3].proxy_path = ".cutmachine/proxies/C8003.mov";
    project.rushes[4].metadata_complete = false;
    project.bin_metadata[project.rushes[5].id] = {
        "retenue\nplan 2", 4, {"itw", "é"}, 7, "Nadia"};
    project.timeline_bin_ids[project.timelines[2].id] = project.bins[1].id;
    DocumentMarker marker;
    marker.name = "raccord";
    marker.time = {100, 25};
    project.timelines[1].markers.push_back(marker);
    Check(project.Validate(error), "fixture is valid: " + error);

    const std::string written = project.SaveToString();
    Check(written.find("\"project_version\":3") != std::string::npos,
          "projects are written as version 3");
    Check(written.find("timeline_snapshots") == std::string::npos,
          "version 3 embeds no whole documents");

    Project reloaded;
    Check(Project::LoadFromString(written, reloaded, error),
          "version 3 loads: " + error);
    Check(reloaded.SaveToString() == written,
          "version 3 round-trips byte for byte");

    // Each part of the file is written by Document's own part writers.
    Check(written.find(document_json::LibraryMediaJson(project.rushes[3])) !=
                  std::string::npos &&
              written.find(document_json::SequenceJson(project.timelines[1])) !=
                  std::string::npos,
          "version 3 parts are Document's canonical parts");

    const std::string versionTwo = VersionTwo(project);
    Project migrated;
    Check(Project::LoadFromString(versionTwo, migrated, error),
          "a version-2 project still loads: " + error);
    Check(migrated.SaveToString() == written,
          "and becomes exactly the same version-3 project");

    Check(written.size() * 5 < versionTwo.size(),
          "11 timelines no longer repeat the library (" +
              std::to_string(written.size()) + " bytes vs " +
              std::to_string(versionTwo.size()) + " in version 2)");
    Project twoTimelines = MakeInterviewShapedProject(405, 2, 24);
    Project twentyTimelines = MakeInterviewShapedProject(405, 20, 24);
    const size_t perTimeline = (twentyTimelines.SaveToString().size() -
                                twoTimelines.SaveToString().size()) /
                               18;
    Check(perTimeline * 10 < twoTimelines.SaveToString().size(),
          "a timeline adds its own content, not a copy of the library (" +
              std::to_string(perTimeline) + " bytes per timeline)");

    // Version 2's own consistency rule still holds for version-2 files.
    std::string disagreeing = versionTwo;
    const std::string path = Escaped(project.rushes[0].path);
    const size_t second = disagreeing.find(path, disagreeing.find(path) + 1);
    Check(second != std::string::npos, "the fixture repeats the library");
    disagreeing.replace(second, path.size(), "elsewhere.MP4");
    Project refused;
    Check(!Project::LoadFromString(disagreeing, refused, error) &&
              error.find("disagree") != std::string::npos,
          "version-2 timelines that disagree on the library are refused");

    Check(!Project::LoadFromString("{\"project_format\":\"cutmachine-project\","
                                   "\"project_version\":4}",
                                   refused, error),
          "an unknown project version is refused");

    std::cout << "All project format tests passed\n";
    return 0;
}
