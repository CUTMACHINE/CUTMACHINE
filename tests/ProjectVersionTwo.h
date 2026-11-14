#pragma once

// S7 -- SAVING_ROADMAP.md. The version-2 project writer as it was before S7:
// one complete document per timeline, each with its own copy of the shared
// parts. Kept so that what reads version 2 is tested against the exact bytes
// projects on disk carry: the project files of project_format_tests, and
// the whole projects that journals written before S6 hold in each entry
// (project_log_migration_tests, DEC24).

#include "Document.h"
#include "DocumentJson.h"
#include "Project.h"

#include <cstddef>
#include <sstream>
#include <string>

namespace project_version_two {

inline std::string Escaped(const std::string& input) {
    std::string output;
    for (const char character : input) {
        switch (character) {
            case '"':
                output += "\\\"";
                break;
            case '\\':
                output += "\\\\";
                break;
            case '\n':
                output += "\\n";
                break;
            default:
                output.push_back(character);
        }
    }
    return output;
}

inline std::string VersionTwo(const Project& project) {
    std::ostringstream output;
    output << "{\n  \"project_format\":\"cutmachine-project\",";
    output << "\n  \"project_version\":2,";
    output << "\n  \"id\":\"" << Escaped(project.id) << "\",";
    output << "\n  \"name\":\"" << Escaped(project.name) << "\",";
    output << "\n  \"active_timeline_id\":\""
           << Escaped(project.active_timeline_id) << "\",";
    output << "\n  \"timeline_snapshots\":[";
    for (size_t index = 0; index < project.timelines.size(); ++index) {
        Document document;
        document.sequence = project.timelines[index];
        document.color_management = project.settings.color_management;
        document.library = project.rushes;
        document.bins = project.bins;
        document.sources = project.sources;
        output << (index == 0 ? "\n" : ",\n") << "    \""
               << Escaped(document.SaveToString()) << "\"";
    }
    output << "\n  ],\n  \"bin_metadata\":[";
    size_t metadataIndex = 0;
    for (const auto& item : project.bin_metadata) {
        output << (metadataIndex++ == 0 ? "\n" : ",\n") << "    {\"item_id\":\""
               << Escaped(item.first) << "\",\"description\":\""
               << Escaped(item.second.description)
               << "\",\"rating\":" << item.second.rating << ",\"tags\":[";
        for (size_t tag = 0; tag < item.second.tags.size(); ++tag) {
            if (tag) output << ',';
            output << "\"" << Escaped(item.second.tags[tag]) << "\"";
        }
        output << "],\"insert_order\":" << item.second.insert_order
               << ",\"display_name\":\"" << Escaped(item.second.display_name)
               << "\"}";
    }
    if (!project.bin_metadata.empty()) output << '\n';
    output << "  ],\n  \"timeline_bin_ids\":[";
    size_t placementIndex = 0;
    for (const auto& item : project.timeline_bin_ids) {
        output << (placementIndex++ == 0 ? "\n" : ",\n")
               << "    {\"timeline_id\":\"" << Escaped(item.first)
               << "\",\"bin_id\":\"" << Escaped(item.second) << "\"}";
    }
    if (!project.timeline_bin_ids.empty()) output << '\n';
    output << "  ]\n}\n";
    return output.str();
}

}  // namespace project_version_two
