// DEC44 -- docs/roadmap/DONNEES-2026-10.md. A capture cut into chunks gives
// back its document byte for byte; the manifest that names them is
// canonical; the chunk codec refuses what it did not write.

#include "ArchiveManifest.h"
#include "DocumentJson.h"
#include "Json.h"
#include "Project.h"
#include "ProjectFixture.h"

#include <cstdint>
#include <exception>
#include <iostream>
#include <random>
#include <set>
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

template <typename Function>
void Test(const std::string& name, Function function) {
    const int before = failures;
    try {
        function();
    } catch (const std::exception& exception) {
        ++failures;
        std::cerr << "FAIL: " << name << ": " << exception.what() << '\n';
    }
    if (failures == before) {
        std::cout << "PASS: " << name << '\n';
    }
}

const char kTitleDefinition[] =
    R"({"width":1920,"height":1080,"fps":25,"duration":60,)"
    R"("root":{"type":"fill","uid":"01KPRD24000000000000000032",)"
    R"("children":[{"type":"text","uid":"01KPRD24000000000000000033",)"
    R"("content":"Titre","style":{"fontSize":80}}]}})";

Project ProjectWithComposition() {
    Project project = MakeInterviewShapedProject(12, 3, 4);
    DocumentComposition composition;
    composition.name = "Titre « Émilie »";
    std::string error;
    if (!mcp_json::Value::Parse(kTitleDefinition, composition.definition,
                                error)) {
        throw std::runtime_error(error);
    }
    project.compositions.push_back(composition);
    return project;
}

std::string Reassembled(const ArchiveSplit& split) {
    std::vector<std::string_view> views(split.parts.begin(), split.parts.end());
    std::string document;
    std::string error;
    if (!AssembleArchiveDocument(split.manifest, split.rest, views, document,
                                 error)) {
        throw std::runtime_error(error);
    }
    return document;
}

// The split always answers, and always gives the document back.
void CheckExact(const std::string& document, const std::string& what) {
    const ArchiveSplit split = SplitArchiveDocument(document);
    Check(Reassembled(split) == document &&
              split.manifest.bytes == static_cast<int64_t>(document.size()) &&
              split.manifest.rest == ArchiveDigest(split.rest),
          what + ": the chunks give the document back");
    for (size_t index = 0; index < split.parts.size(); ++index) {
        Check(split.manifest.parts[index].digest ==
                  ArchiveDigest(split.parts[index]),
              what + ": each part is named by its digest");
    }
    std::string error;
    ArchiveManifest parsed;
    Check(ParseArchiveManifest(SerializeArchiveManifest(split.manifest), parsed,
                               error) &&
              SerializeArchiveManifest(parsed) ==
                  SerializeArchiveManifest(split.manifest),
          what + ": its manifest reads back: " + error);
}

}  // namespace

int main() {
    Test("SHA-256 digests", [] {
        Check(ArchiveDigest("") ==
                  "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b"
                  "7852b855",
              "the digest of nothing");
        Check(ArchiveDigest("abc") ==
                  "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61"
                  "f20015ad",
              "the digest of abc");
        Check(IsArchiveDigest(ArchiveDigest("x")) && !IsArchiveDigest("ABC") &&
                  !IsArchiveDigest(std::string(64, 'G')) &&
                  !IsArchiveDigest(std::string(63, 'a')),
              "a digest is 64 lowercase hexadecimal digits");
    });

    Test("a project is cut into its timelines, compositions and rest", [] {
        const Project project = ProjectWithComposition();
        const std::string document = project.SaveToString();
        const ArchiveSplit split = SplitArchiveDocument(document);
        Check(split.parts.size() == 4 &&
                  split.manifest.parts[0].kind == "composition" &&
                  split.manifest.parts[0].id == project.compositions[0].id,
              "the composition first, as the file writes it");
        for (size_t index = 0; index < 3; ++index) {
            const ArchivePart& part = split.manifest.parts[index + 1];
            Check(part.kind == "timeline" &&
                      part.id == project.timelines[index].id &&
                      split.parts[index + 1] ==
                          document_json::SequenceJson(project.timelines[index]),
                  "timeline " + std::to_string(index) +
                      " is its canonical part, named by its id");
        }
        // The first timeline's id stays: active_timeline_id names it.
        Check(split.rest.find("\"library\"") != std::string::npos &&
                  split.rest.find(project.timelines[2].id) == std::string::npos,
              "the library stays in the rest; the timelines do not");
        CheckExact(document, "a project with a composition");
    });

    Test("an edit in one timeline changes one chunk", [] {
        Project project = MakeInterviewShapedProject(20, 5, 6);
        const ArchiveSplit before =
            SplitArchiveDocument(project.SaveToString());
        project.timelines[2].name = "Variante resserrée";
        const ArchiveSplit after = SplitArchiveDocument(project.SaveToString());
        Check(before.manifest.rest == after.manifest.rest,
              "the rest is the same chunk");
        std::set<size_t> changed;
        for (size_t index = 0; index < before.parts.size(); ++index) {
            if (before.manifest.parts[index].digest !=
                after.manifest.parts[index].digest) {
                changed.insert(index);
            }
        }
        Check(changed == std::set<size_t>{2},
              "only the edited timeline's digest changes");
    });

    Test("documents that do not cut are kept whole, exactly", [] {
        for (const std::string& document :
             {std::string("[]"), std::string("{}"), std::string("\"text\""),
              std::string("{\"timelines\":[]}"),
              std::string("{\"timelines\":{\"a\":1}}"),
              std::string("{\"timelines\":[1,2"),
              std::string("{\"timelines\":[{\"id\":\"x\"}"),
              std::string("not json at all"), std::string()}) {
            const ArchiveSplit split = SplitArchiveDocument(document);
            Check(split.parts.empty() && split.rest == document,
                  "kept whole: '" + document + "'");
            CheckExact(document, "whole '" + document + "'");
        }
    });

    Test("the cut reads the bytes, never reserializes them", [] {
        // Whitespace and order of another writer, strings for timelines (as
        // project version 2 embedded them), a timelines key nested deeper,
        // escapes and braces inside strings, duplicate keys: every one
        // gives the same bytes back.
        const std::string documents[] = {
            "{ \"name\" : \"a\" , \"timelines\" : [ {\"id\":\"t1\",\"x\":[1,"
            "{\"timelines\":[9]}]} ,\n\t{ \"id\" : \"t\\u0032\" } ] }",
            "{\"timelines\":[\"{\\\"version\\\":7}\",\"[]\"],\"compositions"
            "\":[{\"id\":\"c\",\"definition\":{\"text\":\"}]{[\\\"\"}}]}",
            "{\"timelines\":[{\"id\":1}],\"timelines\":[{\"id\":\"b\"}]}",
            "{\"a\":{\"timelines\":[{\"id\":\"nested\"}]},\"b\":true,"
            "\"c\":null,\"d\":-1.5e3}",
            "{\"timel\\u0069nes\":[{\"id\":\"escaped key\"}]}"};
        for (const std::string& document : documents) {
            CheckExact(document, document.substr(0, 40));
        }
        const ArchiveSplit first = SplitArchiveDocument(documents[0]);
        Check(first.parts.size() == 2 && first.manifest.parts[1].id == "t2",
              "an id is read as JSON, its escapes decoded");
        const ArchiveSplit strings = SplitArchiveDocument(documents[1]);
        Check(strings.parts.size() == 3 && strings.manifest.parts[0].id.empty(),
              "an element that is a string is a part without id");
        Check(SplitArchiveDocument(documents[3]).parts.empty(),
              "only the top level is cut");
        Check(SplitArchiveDocument(documents[4]).parts.empty(),
              "a key spelled with escapes is not the engine's: kept whole");
    });

    Test("damaged and truncated projects never lose a byte", [] {
        const std::string document = ProjectWithComposition().SaveToString();
        std::mt19937 random(44);
        std::uniform_int_distribution<size_t> position(0, document.size());
        for (int trial = 0; trial < 300; ++trial) {
            std::string damaged = document;
            switch (trial % 3) {
                case 0:
                    damaged.resize(position(random));
                    break;
                case 1:
                    damaged.insert(position(random), "\"");
                    break;
                default:
                    damaged.erase(position(random) % damaged.size(), 1);
                    break;
            }
            CheckExact(damaged, "damaged " + std::to_string(trial));
        }
    });

    Test("the manifest is canonical, and pinned", [] {
        ArchiveManifest manifest;
        manifest.bytes = 120;
        manifest.rest = ArchiveDigest("rest");
        manifest.parts.push_back(
            {"composition", "01K", 10, ArchiveDigest("c")});
        manifest.parts.push_back(
            {"timeline", "\xc3\x89\"", 50, ArchiveDigest("t")});
        const std::string written = SerializeArchiveManifest(manifest);
        Check(written ==
                  "{\"format\":\"cutmachine.archive-manifest.v1\",\"bytes\":"
                  "120,\"rest\":\"" +
                      ArchiveDigest("rest") +
                      "\",\"parts\":[{\"kind\":\"composition\",\"id\":\"01K\","
                      "\"at\":10,\"digest\":\"" +
                      ArchiveDigest("c") +
                      "\"},{\"kind\":\"timeline\",\"id\":\"\xc3\x89\\\"\","
                      "\"at\":50,\"digest\":\"" +
                      ArchiveDigest("t") + "\"}]}",
              "the bytes a manifest is written as: " + written);
        ArchiveManifest parsed;
        std::string error;
        Check(ParseArchiveManifest(written, parsed, error) &&
                  SerializeArchiveManifest(parsed) == written &&
                  parsed.parts.size() == 2 &&
                  parsed.parts[1].id == "\xc3\x89\"",
              "it reads back to the same bytes: " + error);
        ArchiveManifest empty;
        empty.rest = ArchiveDigest("");
        Check(SerializeArchiveManifest(empty) ==
                  "{\"format\":\"cutmachine.archive-manifest.v1\",\"bytes\":0,"
                  "\"rest\":\"" +
                      ArchiveDigest("") + "\",\"parts\":[]}",
              "a document kept whole");
    });

    Test("the manifest reader refuses what no writer produces", [] {
        ArchiveManifest base;
        base.bytes = 100;
        base.rest = ArchiveDigest("rest");
        base.parts.push_back({"timeline", "a", 10, ArchiveDigest("a")});
        base.parts.push_back({"timeline", "b", 40, ArchiveDigest("b")});
        const std::string valid = SerializeArchiveManifest(base);
        const auto replaced = [&](const std::string& from,
                                  const std::string& to) {
            std::string text = valid;
            text.replace(text.find(from), from.size(), to);
            return text;
        };
        const std::vector<std::pair<std::string, std::string>> cases = {
            {replaced("archive-manifest.v1", "archive-manifest.v2"),
             "another format"},
            {replaced("\"bytes\":100", "\"bytes\":-1"), "a negative size"},
            {replaced(ArchiveDigest("rest"), "abc"), "a rest without digest"},
            {replaced("\"kind\":\"timeline\"", "\"kind\":\"library\""),
             "an unknown kind"},
            {replaced("\"at\":40", "\"at\":10"), "two parts at one offset"},
            {replaced("\"at\":40", "\"at\":5"), "parts out of order"},
            {replaced("\"at\":40", "\"at\":100"), "a part past the end"},
            {replaced("\"bytes\":100,", ""), "a missing key"},
            {replaced("\"bytes\":100,", "\"bytes\":100,\"extra\":1,"),
             "an unknown key"},
            {replaced("\"bytes\":100", "\"bytes\":\"100\""), "a wrong type"},
            {"[]", "not an object"},
            {"{", "not JSON"}};
        for (const auto& [text, what] : cases) {
            ArchiveManifest parsed;
            std::string error;
            Check(!ParseArchiveManifest(text, parsed, error) && !error.empty(),
                  "refused: " + what);
        }
        ArchiveManifest parsed;
        std::string error;
        Check(ParseArchiveManifest(valid, parsed, error),
              "the valid one reads: " + error);
    });

    Test("reassembly refuses a manifest that does not fit", [] {
        ArchiveManifest manifest;
        manifest.bytes = 6;
        manifest.parts.push_back({"timeline", "", 2, ""});
        std::string document;
        std::string error;
        Check(AssembleArchiveDocument(manifest, "abcd", {"XY"}, document,
                                      error) &&
                  document == "abXYcd",
              "a part goes back at its offset");
        Check(!AssembleArchiveDocument(manifest, "abcd", {}, document, error),
              "a missing part is refused");
        manifest.bytes = 7;
        Check(
            !AssembleArchiveDocument(manifest, "abcd", {"XY"}, document, error),
            "a wrong length is refused");
        manifest.bytes = 6;
        manifest.parts[0].at = 9;
        Check(
            !AssembleArchiveDocument(manifest, "abcd", {"XY"}, document, error),
            "an offset past the rest is refused");
    });

    Test("the chunk codec", [] {
        const std::string document = ProjectWithComposition().SaveToString();
        std::string frame;
        std::string back;
        std::string error;
        Check(CompressArchiveChunk(document, frame, error) &&
                  frame.size() * 4 < document.size() &&
                  DecompressArchiveChunk(frame, document.size(), back, error) &&
                  back == document,
              "a project compresses and comes back: " + error);
        Check(CompressArchiveChunk("", frame, error) &&
                  DecompressArchiveChunk(frame, 0, back, error) && back.empty(),
              "an empty chunk too: " + error);
        Check(CompressArchiveChunk(document, frame, error) &&
                  !DecompressArchiveChunk(frame, document.size() + 1, back,
                                          error),
              "a frame whose size is not the recorded one is refused");
        Check(!DecompressArchiveChunk(frame.substr(0, frame.size() / 2),
                                      document.size(), back, error),
              "a truncated frame is refused");
        Check(
            !DecompressArchiveChunk(frame + "x", document.size(), back, error),
            "bytes after the frame are refused");
        Check(!DecompressArchiveChunk("not zstd", 8, back, error),
              "bytes that are no frame are refused");
    });

    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    return 0;
}
