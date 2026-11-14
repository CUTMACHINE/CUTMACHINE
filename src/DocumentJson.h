#pragma once

// S6/S7 -- SAVING_ROADMAP.md. The canonical JSON of each part of a Document,
// byte for byte what Document::SaveToString writes for that part.
//
// A project history entry records only the parts an operation changed
// (ProjectDelta.h), and a version-3 project file stores the shared library
// once beside its timelines (Project::SaveToString). Both need a part on its
// own, and both must read it back with exactly the rules a whole document is
// read with: a second, hand-written serializer would drift the first time a
// clip gains a field, and would then drop that field from every undo without
// any test noticing.

#include "Document.h"
#include "Project.h"

#include <string>
#include <vector>

namespace document_json {

std::string SequenceJson(const DocumentSequence& sequence);
std::string ColorManagementJson(const ColorManagementSettings& settings);
std::string LibraryMediaJson(const LibraryMedia& media);
std::string BinJson(const DocumentBin& bin);
std::string SourceJson(const DocumentSource& source);
std::string BinMetadataJson(const ProjectBinMetadata& metadata);
std::string MediaRateJson(const MediaRate& rate);

// Parts as written by the functions above. An empty `sequence` stands for no
// sequence at all: the parsed document then carries an empty, id-less one.
// An empty `color_management` keeps the defaults.
struct DocumentParts {
    std::string sequence;
    std::string color_management;
    std::vector<std::string> library;
    std::vector<std::string> bins;
    std::vector<std::string> sources;
};

// Reads the parts through Document::LoadFromString, without whole-document
// validation: parts are not a document, and whoever reassembles them
// validates the result (Project::Validate).
bool ParseDocumentParts(const DocumentParts& parts, Document& output,
                        std::string& error);

// A delta's own records, as BinMetadataJson and MediaRateJson write them.
// Strict, unlike a document: every key is required and no other is
// accepted. `context` prefixes the error, which names the offending key.
bool ParseBinMetadata(const std::string& json, const std::string& context,
                      ProjectBinMetadata& output, std::string& error);
bool ParseMediaRate(const std::string& json, const std::string& context,
                    MediaRate& output, std::string& error);

}  // namespace document_json
