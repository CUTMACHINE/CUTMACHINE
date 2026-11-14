// PRD18 -- ROADMAP.md (ACP4, docs/roadmap/ACP-2026-09.md). The procedures
// the binary carries (Procedures.h).
//
// The build embeds the repository's skills (cmake/EmbedProcedures.cmake).
// This suite holds the binary to them: every embedded file is the
// repository's, byte for byte, and no text file of a skill is left out.
// Then the reading: a name, a path, a folder, a path that tries to leave its
// procedure, the links each Markdown file holds, and the front matter a
// description comes from. Only this test is told where the skills are
// (CUTMACHINE_SKILLS_DIR): the binary reads no directory.
//
// Plain C++: no AppKit, no FFmpeg, no network.

#include "Procedures.h"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

int failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

bool Contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

std::string Read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream bytes;
    bytes << input.rdbuf();
    return bytes.str();
}

// Well-formed UTF-8: what a JSON answer can carry as it is.
bool IsUtf8(const std::string& text) {
    size_t index = 0;
    while (index < text.size()) {
        const auto byte = static_cast<unsigned char>(text[index]);
        size_t length = 0;
        if (byte < 0x80) {
            length = 1;
        } else if ((byte & 0xE0) == 0xC0 && byte >= 0xC2) {
            length = 2;
        } else if ((byte & 0xF0) == 0xE0) {
            length = 3;
        } else if ((byte & 0xF8) == 0xF0 && byte <= 0xF4) {
            length = 4;
        } else {
            return false;
        }
        if (index + length > text.size()) {
            return false;
        }
        for (size_t next = 1; next < length; ++next) {
            if ((static_cast<unsigned char>(text[index + next]) & 0xC0) !=
                0x80) {
                return false;
            }
        }
        index += length;
    }
    return true;
}

const std::filesystem::path kSkills = CUTMACHINE_SKILLS_DIR;
// The most one file may weigh: a procedure is read into a model's context
// whole, and the binary carries every one.
constexpr size_t kMaxFileBytes = size_t{64} * 1024;

using FileKey = std::pair<std::string, std::string>;

// What the build should have embedded, read from the repository the way
// cmake/EmbedProcedures.cmake reads it: in each directory holding a
// SKILL.md, the .md, .json and .py files, nothing hidden.
std::set<FileKey> RepositoryFiles() {
    std::set<FileKey> files;
    for (const auto& directory : std::filesystem::directory_iterator(kSkills)) {
        if (!directory.is_directory() ||
            !std::filesystem::is_regular_file(directory.path() / "SKILL.md")) {
            continue;
        }
        const std::string procedure = directory.path().filename().string();
        for (const auto& entry :
             std::filesystem::recursive_directory_iterator(directory.path())) {
            if (!entry.is_regular_file()) {
                continue;
            }
            const std::string path = entry.path()
                                         .lexically_relative(directory.path())
                                         .generic_string();
            const std::string extension = entry.path().extension().string();
            const bool hidden = path[0] == '.' || Contains(path, "/.");
            if (!hidden && (extension == ".md" || extension == ".json" ||
                            extension == ".py")) {
                files.insert({procedure, path});
            }
        }
    }
    return files;
}

std::map<FileKey, std::string> EmbeddedFiles() {
    std::map<FileKey, std::string> files;
    size_t count = 0;
    const EmbeddedProcedureFile* table = EmbeddedProcedureFiles(count);
    for (size_t index = 0; index < count; ++index) {
        files[{table[index].procedure, table[index].path}] =
            std::string(reinterpret_cast<const char*>(table[index].bytes),
                        table[index].size);
    }
    return files;
}

void CheckEmbeddedMatchesRepository() {
    const std::set<FileKey> repository = RepositoryFiles();
    const std::map<FileKey, std::string> embedded = EmbeddedFiles();
    size_t total = 0;
    for (const auto& [key, bytes] : embedded) {
        const std::string label = key.first + "/" + key.second;
        Check(repository.count(key) == 1,
              label + " is embedded and is a text file of a skill");
        Check(bytes == Read(kSkills / key.first / key.second),
              label + " is the repository's file, byte for byte");
        Check(bytes.size() <= kMaxFileBytes,
              label + " weighs at most 64 KiB: " +
                  std::to_string(bytes.size()) + " bytes");
        Check(bytes.find('\0') == std::string::npos && IsUtf8(bytes),
              label + " is text: UTF-8, no NUL");
        total += bytes.size();
    }
    for (const FileKey& key : repository) {
        Check(embedded.count(key) == 1,
              key.first + "/" + key.second + " is embedded");
    }
    Check(embedded.size() == repository.size() && !embedded.empty(),
          "the binary carries every text file of the skills: " +
              std::to_string(embedded.size()) + " of " +
              std::to_string(repository.size()));
    std::cout << embedded.size() << " files embedded, " << total << " bytes\n";
}

void CheckListing() {
    const std::vector<Procedure> procedures = ListProcedures();
    std::set<std::string> names;
    for (const Procedure& procedure : procedures) {
        names.insert(procedure.name);
    }
    Check(
        names.count("monter-une-itw") == 1 && names.count("motion-design") == 1,
        "the list names the interview and motion design procedures");
    std::set<std::string> skills;
    for (const auto& directory : std::filesystem::directory_iterator(kSkills)) {
        if (std::filesystem::is_regular_file(directory.path() / "SKILL.md")) {
            skills.insert(directory.path().filename().string());
        }
    }
    Check(names == skills, "the list is the repository's skills, no more");
    const std::map<FileKey, std::string> embedded = EmbeddedFiles();
    for (const Procedure& procedure : procedures) {
        const std::string skill =
            Read(kSkills / procedure.name / kProcedureMainFile);
        Check(!procedure.description.empty() &&
                  procedure.description ==
                      SkillFrontMatterField(skill, "description"),
              procedure.name + " is described by its SKILL.md front matter");
        Check(SkillFrontMatterField(skill, "name") == procedure.name,
              procedure.name + "'s front matter names its directory");
        Check(!procedure.files.empty() &&
                  procedure.files[0].path == kProcedureMainFile,
              procedure.name + " lists SKILL.md first");
        for (const ProcedureFile& file : procedure.files) {
            const auto found = embedded.find({procedure.name, file.path});
            Check(
                found != embedded.end() && found->second.size() == file.bytes,
                procedure.name + "/" + file.path + " is listed with its size");
        }
    }
}

ProcedureText ReadOk(const std::string& name, const std::string& path) {
    ProcedureText file;
    std::string errorName;
    std::string message;
    Check(ReadProcedureFile(name, path, file, errorName, message),
          name + " " + path + " is read: " + errorName + " " + message);
    return file;
}

// The refusal of a read: its error name, and its sentence.
std::pair<std::string, std::string> Refusal(const std::string& name,
                                            const std::string& path) {
    ProcedureText file;
    std::string errorName;
    std::string message;
    Check(!ReadProcedureFile(name, path, file, errorName, message) &&
              file.text.empty(),
          name + " " + path + " is refused, and nothing is read");
    return {errorName, message};
}

void CheckReading() {
    const ProcedureText skill = ReadOk("monter-une-itw", "SKILL.md");
    Check(skill.name == "monter-une-itw" && skill.path == "SKILL.md" &&
              skill.text == Read(kSkills / "monter-une-itw" / "SKILL.md"),
          "a procedure's SKILL.md is read as the skill holds it");
    Check(!skill.files.empty() && skill.files[0].path == "SKILL.md",
          "the read carries the procedure's files");
    Check(ReadOk("monter-une-itw", "mots-et-bornes.md").text ==
              Read(kSkills / "monter-une-itw" / "mots-et-bornes.md"),
          "a link of a SKILL.md is the file to ask for");
    Check(ReadOk("monter-une-itw", "./mots-et-bornes.md").path ==
              "mots-et-bornes.md",
          "a ./ before the path changes nothing");
    Check(ReadOk("monter-une-itw", "").path == "SKILL.md",
          "no path is the SKILL.md");
    const ProcedureText otherCase = ReadOk("Monter-Une-ITW", "skill.md");
    Check(otherCase.name == "monter-une-itw" && otherCase.path == "SKILL.md",
          "a name and a path in another case are read under their own "
          "spelling");
    Check(ReadOk("motion-design", "references/motifs.md").text ==
              Read(kSkills / "motion-design" / "references" / "motifs.md"),
          "a file of a subfolder is read by its path");

    const auto unknown = Refusal("monter-un-film", "SKILL.md");
    Check(unknown.first == "UnknownProcedure" &&
              Contains(unknown.second, "unknown procedure 'monter-un-film'"),
          "an unknown procedure is refused by name: " + unknown.second);
    for (const Procedure& procedure : ListProcedures()) {
        Check(Contains(unknown.second, procedure.name),
              "the refusal names " + procedure.name);
    }
    const auto absent = Refusal("monter-une-itw", "absent.md");
    Check(absent.first == "UnknownProcedureFile" &&
              Contains(absent.second, "has no file 'absent.md'") &&
              Contains(absent.second, "mots-et-bornes.md"),
          "an unknown file is refused with the procedure's files: " +
              absent.second);
    for (const std::string& folder : {"exemples/", "exemples"}) {
        const auto refused = Refusal("motion-design", folder);
        Check(refused.first == "UnknownProcedureFile" &&
                  Contains(refused.second, "is a folder") &&
                  Contains(refused.second, "exemples/compteur.json") &&
                  !Contains(refused.second, "references/motifs.md"),
              "a folder is answered with its files: " + refused.second);
    }

    const auto otherProcedure =
        Refusal("monter-un-reel", "../monter-une-itw/resolve.md");
    Check(otherProcedure.first == "ValidationFailed" &&
              Contains(otherProcedure.second, "leaves procedure") &&
              Contains(otherProcedure.second,
                       "file 'resolve.md' of procedure 'monter-une-itw'"),
          "a path to another procedure's file is refused with the call to "
          "make: " +
              otherProcedure.second);
    const auto fromSubfolder =
        Refusal("motion-design", "../exemples/compteur.json");
    Check(fromSubfolder.first == "ValidationFailed" &&
              Contains(fromSubfolder.second,
                       "reads as its file 'exemples/compteur.json'"),
          "a link copied from a subfolder is refused with the file it "
          "means: " +
              fromSubfolder.second);
    for (const std::string& outside :
         {"../../../docs/MOTION_FORMAT.md", "../../../../etc/passwd",
          "references/../../monter-une-itw/../../README.md"}) {
        const auto refused = Refusal("motion-design", outside);
        Check(refused.first == "ValidationFailed" &&
                  Contains(refused.second,
                           "nothing outside the procedures "
                           "is served"),
              "a path out of the procedures is refused: " + refused.second);
    }
    Check(Refusal("motion-design", "references/../SKILL.md").first ==
              "ValidationFailed",
          "any .. is refused, even one that would come back inside");
    for (const std::string& absolute :
         {"/etc/passwd", "/Users/x/.claude/skills/motion-design/SKILL.md",
          "references\\motifs.md"}) {
        Check(Refusal("motion-design", absolute).first == "ValidationFailed",
              "an absolute path, or one written with backslashes, is "
              "refused: " +
                  absolute);
    }
}

const ProcedureLink* FindLink(const ProcedureText& file,
                              const std::string& link) {
    for (const ProcedureLink& candidate : file.links) {
        if (candidate.link == link) {
            return &candidate;
        }
    }
    return nullptr;
}

void CheckLinks() {
    const ProcedureText motifs =
        ReadOk("motion-design", "references/motifs.md");
    const ProcedureLink* example =
        FindLink(motifs, "../exemples/compteur.json");
    Check(example != nullptr && example->procedure == "motion-design" &&
              example->file == "exemples/compteur.json",
          "a link of a subfolder is resolved from that subfolder");
    const ProcedureLink* subtitles =
        FindLink(motifs, "../../monter-une-itw/sous-titres.md");
    Check(subtitles != nullptr && subtitles->procedure == "monter-une-itw" &&
              subtitles->file == "sous-titres.md",
          "a link to another procedure names it and its file");
    const ProcedureLink* format =
        FindLink(motifs, "../../../../docs/MOTION_FORMAT.md");
    Check(format != nullptr && format->procedure.empty(),
          "a link out of the procedures is said not to be served");
    const ProcedureText direction =
        ReadOk("motion-design", "references/direction.md");
    const ProcedureLink* sibling = FindLink(direction, "motifs.md");
    Check(sibling != nullptr && sibling->file == "references/motifs.md",
          "a link between two files of a subfolder names the file's path");
    const ProcedureText motion = ReadOk("motion-design", "SKILL.md");
    const ProcedureLink* folder = FindLink(motion, "exemples/");
    Check(folder != nullptr && folder->procedure == "motion-design" &&
              folder->file.empty() && folder->folder == "exemples",
          "a link to a folder names the folder");
    Check(std::count_if(motion.links.begin(), motion.links.end(),
                        [](const ProcedureLink& link) {
                            return link.link == "references/direction.md";
                        }) == 1,
          "a link given three times is listed once");
    Check(ReadOk("motion-design", "exemples/compteur.json").links.empty(),
          "a file that is not Markdown has no links");

    // Every link of every procedure leads to a file or a folder the binary
    // serves, or out of the skills altogether: none leads to nothing.
    size_t links = 0;
    for (const Procedure& procedure : ListProcedures()) {
        for (const ProcedureFile& listed : procedure.files) {
            const ProcedureText file = ReadOk(procedure.name, listed.path);
            for (const ProcedureLink& link : file.links) {
                ++links;
                const std::string label =
                    procedure.name + "/" + listed.path + " -> " + link.link;
                if (!link.procedure.empty()) {
                    if (!link.file.empty()) {
                        ProcedureText target;
                        std::string errorName;
                        std::string message;
                        Check(ReadProcedureFile(link.procedure, link.file,
                                                target, errorName, message),
                              label + " can be read as it is given");
                    }
                    continue;
                }
                const std::filesystem::path from =
                    (kSkills / procedure.name / listed.path).parent_path();
                const std::string target =
                    std::filesystem::weakly_canonical(from / link.link)
                        .generic_string();
                std::string skills =
                    std::filesystem::weakly_canonical(kSkills).generic_string();
                skills += '/';
                std::string why = label;
                why +=
                    " is left unserved only because it leads out of the "
                    "skills: ";
                why += target;
                Check(target.compare(0, skills.size(), skills) != 0, why);
            }
        }
    }
    Check(links > 20,
          "the procedures' links were checked: " + std::to_string(links));
}

void CheckFrontMatter() {
    const auto field = [](const std::string& text) {
        return SkillFrontMatterField(text, "description");
    };
    Check(field("---\nname: x\ndescription: Monter une interview : le propos."
                "\n---\n# Titre\n") == "Monter une interview : le propos.",
          "a plain scalar, a colon inside it");
    Check(field("---\ndescription: \"Dit \\\"oui\\\", \\\\ et "
                "\\u00e9t\\u00e9\"\n---\n") ==
              "Dit \"oui\", \\ et \xC3\xA9t\xC3\xA9",
          "a double-quoted scalar, its escapes read");
    Check(field("---\ndescription: 'l''image'\n---\n") == "l'image",
          "a single-quoted scalar");
    Check(field("---\ndescription: >\n  ligne un\n  ligne deux\n\n  "
                "second\n---\n") == "ligne un ligne deux\nsecond",
          "a folded block");
    Check(field("---\ndescription: |-\n  a\n  b\n---\n") == "a\nb",
          "a literal block");
    Check(field("---\ndescription: début\n  suite\nname: x\n---\n") ==
              "début suite",
          "a plain scalar over two lines, up to the next key");
    Check(field("---\ndescription: \"début\n  suite\"\n---\n") == "début suite",
          "a quoted scalar over two lines");
    Check(field("---\ndescription: texte # une note\n---\n") == "texte",
          "a comment after a plain scalar");
    Check(field("---\r\ndescription: a\r\n---\r\n") == "a", "CR LF line ends");
    Check(field("# Titre\ndescription: x\n").empty() &&
              field("---\ndescription: x\n").empty() &&
              field("---\ndescriptions: x\n---\n").empty() &&
              field("---\nname: x\n---\n").empty() && field("").empty(),
          "no front matter, one never closed, another key or none: empty");
    Check(SkillFrontMatterField("---\nname: monter\n---\n", "name") == "monter",
          "any field is read");
}

void CheckJson() {
    const mcp_json::Value listing = ProcedureListingJson();
    const mcp_json::Value* procedures = listing.Find("procedures");
    Check(procedures != nullptr && procedures->IsArray() &&
              procedures->AsArray().size() == ListProcedures().size(),
          "the listing holds every procedure");
    if (procedures != nullptr && procedures->IsArray()) {
        for (const mcp_json::Value& procedure : procedures->AsArray()) {
            const mcp_json::Value* files = procedure.Find("files");
            Check(procedure.Find("name") != nullptr &&
                      procedure.Find("description") != nullptr &&
                      files != nullptr && files->IsArray() &&
                      !files->AsArray().empty() &&
                      files->AsArray()[0].Find("path") != nullptr &&
                      files->AsArray()[0].Find("bytes") != nullptr,
                  "each procedure is listed with its name, description and "
                  "files: " +
                      procedure.Dump().substr(0, 200));
        }
    }

    mcp_json::Value answer;
    std::string errorName;
    std::string message;
    Check(ProcedureFileJson("motion-design", "references/motifs.md", answer,
                            errorName, message),
          "a file answers: " + message);
    const mcp_json::Value* text = answer.Find("text");
    const mcp_json::Value* links = answer.Find("links");
    Check(answer.Find("name") != nullptr &&
              answer.Find("name")->AsString() == "motion-design" &&
              answer.Find("file") != nullptr &&
              answer.Find("file")->AsString() == "references/motifs.md" &&
              answer.Find("files") != nullptr && text != nullptr &&
              text->AsString() ==
                  Read(kSkills / "motion-design" / "references" / "motifs.md"),
          "the answer names the file and carries its text");
    const std::string dumped = links != nullptr ? links->Dump() : std::string();
    Check(Contains(dumped,
                   R"({"link":"../exemples/compteur.json","name":)"
                   R"("motion-design","file":"exemples/compteur.json"})") &&
              Contains(dumped, R"({"link":"../../../../docs/MOTION_FORMAT.md",)"
                               R"("served":false})"),
          "the answer gives each link resolved: " + dumped.substr(0, 300));
    Check(ProcedureFileJson("motion-design", "exemples/compteur.json", answer,
                            errorName, message) &&
              answer.Find("links") == nullptr,
          "a file without links answers without the key");
    Check(
        !ProcedureFileJson("absent", "SKILL.md", answer, errorName, message) &&
            errorName == "UnknownProcedure",
        "an unknown procedure is refused in the JSON answer too");
}

}  // namespace

int main() {
    try {
        CheckEmbeddedMatchesRepository();
        CheckListing();
        CheckReading();
        CheckLinks();
        CheckFrontMatter();
        CheckJson();
    } catch (const std::exception& failure) {
        // The repository's skills are read from disk: a missing directory
        // throws.
        std::cerr << "FAIL: the procedure tests threw: " << failure.what()
                  << '\n';
        return 1;
    }
    if (failures != 0) {
        std::cerr << failures << " assertion(s) failed\n";
        return 1;
    }
    std::cout << "All procedure tests passed\n";
    return 0;
}
