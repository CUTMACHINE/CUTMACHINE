// PRD2 -- docs/roadmap/PRODUIT-2026-10.md. FFmpeg checked before it serves.
//
// The binary here is a shell script named ffmpeg that answers `-version`,
// `-filters` and `-encoders` with the lists it was written with, and writes
// every call it receives to a log. That log is the proof the ticket asks
// for: a tool that needs a filter or an encoder the binary lacks is refused
// by name, and the log holds the three listings and nothing else.
//
// Covered: reading the listings as FFmpeg prints them; reading a command
// line for the capabilities it uses; the refusal at the launcher, in the
// caption measurement and in the exporter; the probe made once per binary
// and again when the binary changes; a binary that cannot be probed; and
// the diagnosis on its two surfaces, `--check-ffmpeg` and the answers of
// open_project and create_project.
//
// Plain C++ and POSIX: no AppKit, no FFmpeg, no network.

#include "CaptionGeneration.h"
#include "Cli.h"
#include "Export.h"
#include "FfmpegCapabilities.h"
#include "Json.h"
#include "McpProjectBackend.h"
#include "McpTools.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "TestSupport.h"

namespace {

using mcp_json::Value;

bool Contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

// The listings as FFmpeg 7 prints them, legends included.
std::string FiltersListing(const std::vector<std::string>& filters) {
    std::string text =
        "Filters:\n"
        "  T.. = Timeline support\n"
        "  .S. = Slice threading\n"
        "  ..C = Command support\n"
        "  A = Audio input/output\n"
        "  V = Video input/output\n"
        "  N = Dynamic number and/or type of input/output\n"
        "  | = Source or sink filter\n"
        " ... abench            A->A       Benchmark part of a filtergraph.\n"
        " TSC scale             V->V       Scale the input video size.\n"
        " ... color             |->V       Provide an uniformly colored "
        "input.\n";
    for (const std::string& filter : filters) {
        text += " T.C " + filter + "          V->V       A filter.\n";
    }
    return text;
}

std::string EncodersListing(const std::vector<std::string>& encoders) {
    std::string text =
        "Encoders:\n"
        " V..... = Video\n"
        " A..... = Audio\n"
        " S..... = Subtitle\n"
        " .F.... = Frame-level multithreading\n"
        " ..S... = Slice-level multithreading\n"
        " ...X.. = Codec is experimental\n"
        " ....B. = Supports draw_horiz_band\n"
        " .....D = Supports direct rendering method 1\n"
        " ------\n"
        " V....D ffv1                 FFmpeg video codec #1\n"
        " A....D aac                  AAC (Advanced Audio Coding)\n";
    for (const std::string& encoder : encoders) {
        text += " V....D " + encoder + "              An encoder (codec x)\n";
    }
    return text;
}

const std::vector<std::string> kEveryEncoder = {"h264_videotoolbox",
                                                "hevc_videotoolbox", "libx264",
                                                "libx265", "prores_ks"};

// A script named ffmpeg that lists `filters` and `encoders`, logs every
// call to `log`, and succeeds at whatever else it is asked.
void WriteFakeFfmpeg(const std::filesystem::path& path,
                     const std::filesystem::path& log,
                     const std::vector<std::string>& filters,
                     const std::vector<std::string>& encoders,
                     const std::string& onFilters = "exit 0") {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream script(path, std::ios::trunc);
    script << "#!/bin/sh\n"
           << "echo \"$*\" >> '" << log.string() << "'\n"
           << "case \"$2\" in\n"
           << "-version) echo 'ffmpeg version 7.1-test Copyright (c) the "
              "FFmpeg developers'; exit 0;;\n"
           << "-filters) cat <<'LISTING'\n"
           << FiltersListing(filters) << "LISTING\n"
           << onFilters << ";;\n"
           << "-encoders) cat <<'LISTING'\n"
           << EncodersListing(encoders) << "LISTING\n"
           << "exit 0;;\n"
           << "esac\n"
           << "exit 0\n";
    script.close();
    chmod(path.c_str(), 0755);
}

std::vector<std::string> Lines(const std::filesystem::path& path) {
    std::vector<std::string> lines;
    std::ifstream input(path);
    std::string line;
    while (std::getline(input, line)) {
        lines.push_back(line);
    }
    return lines;
}

const std::vector<std::string> kProbeCalls = {
    "-hide_banner -version", "-hide_banner -filters", "-hide_banner -encoders"};

// The engine's table and a filter row: how a graph is read for a filter,
// though the engine needs none today (LG6).
std::vector<const ffmpeg::Capability*> UsedWithFilter(
    const std::vector<std::string>& arguments) {
    static const std::vector<ffmpeg::Capability> table = [] {
        std::vector<ffmpeg::Capability> rows = {
            {ffmpeg::CapabilityKind::kFilter, "drawtext",
             "text cannot be drawn", "--enable-libfreetype"}};
        for (const ffmpeg::Capability& row : ffmpeg::KnownCapabilities())
            rows.push_back(row);
        return rows;
    }();
    return ffmpeg::CapabilitiesUsed(arguments, table);
}

std::vector<const ffmpeg::Capability*> Used(
    const std::vector<std::string>& arguments) {
    return ffmpeg::CapabilitiesUsed(arguments);
}

std::string Names(const std::vector<const ffmpeg::Capability*>& used) {
    std::string names;
    for (const ffmpeg::Capability* capability : used) {
        names += (names.empty() ? "" : ",") + std::string(capability->name);
    }
    return names;
}

void TestReading() {
    Test("the listings are read as FFmpeg prints them", [] {
        const ffmpeg::Probe probe = ffmpeg::ParseProbe(
            "/opt/ffmpeg",
            "ffmpeg version 7.1.1 Copyright (c) 2000-2025 the FFmpeg "
            "developers\nbuilt with Apple clang\n",
            FiltersListing({"drawtext"}), EncodersListing({"libx264"}));
        Check(probe.answered && probe.failure.empty(), "the probe answered");
        Check(probe.version == "7.1.1",
              "the version is read: " + probe.version);
        Check(probe.filters.count("drawtext") == 1 &&
                  probe.filters.count("scale") == 1 &&
                  probe.filters.count("color") == 1,
              "filters are read, sources included");
        Check(probe.filters.count("=") == 0 &&
                  probe.filters.count("Timeline") == 0 &&
                  probe.filters.size() == 4,
              "the legend is not a filter: " +
                  std::to_string(probe.filters.size()));
        Check(probe.encoders.count("libx264") == 1 &&
                  probe.encoders.count("aac") == 1 &&
                  probe.encoders.size() == 3,
              "encoders are read, the legend is not one: " +
                  std::to_string(probe.encoders.size()));
        for (const ffmpeg::Capability& capability :
             ffmpeg::KnownCapabilities()) {
            const std::string name = capability.name;
            Check(probe.Has(capability) == (name == "libx264"),
                  "Has() answers for " + name);
        }
    });

    Test("a listing that cannot be read is not an FFmpeg without filters", [] {
        const ffmpeg::Probe probe =
            ffmpeg::ParseProbe("/opt/ffmpeg", "something else\n",
                               "no filter here\n", EncodersListing({}));
        Check(!probe.answered && Contains(probe.failure, "listed no filter"),
              "the probe did not answer: " + probe.failure);
        Check(probe.version.empty(), "and has no version");
    });

    Test("the table names what each lack prevents", [] {
        const auto& table = ffmpeg::KnownCapabilities();
        Check(table.size() == 5, "five capabilities are known");
        std::string names;
        for (const ffmpeg::Capability& capability : table) {
            names += std::string(ffmpeg::KindName(capability.kind)) + ":" +
                     capability.name + " ";
            Check(std::string(capability.prevents).size() > 20 &&
                      std::string(capability.build_option).rfind("--", 0) == 0,
                  std::string(capability.name) +
                      " says what it prevents and how to get it");
        }
        // LG6 -- docs/roadmap/LEGENDES-2026-10.md: the captions are drawn
        // by Core Text, and drawtext left the table.
        Check(names ==
                  "encoder:h264_videotoolbox encoder:hevc_videotoolbox "
                  "encoder:libx264 encoder:libx265 encoder:prores_ks ",
              "the encoders, and no filter since drawtext left: " + names);
    });

    Test("a command line is read for what it uses", [] {
        Check(Names(Used({"ffmpeg", "-i", "in.mov", "-c:v", "libx264", "-c:a",
                          "aac", "out.mp4"})) == "libx264",
              "the encoder after -c:v");
        Check(Names(Used({"-i", "in.mov", "-vcodec", "prores_ks",
                          "out.mov"})) == "prores_ks",
              "the encoder after -vcodec, without the binary first");
        Check(Used({"-c:v", "libx264", "-i", "in.mkv", "-c:v", "copy", "out"})
                  .empty(),
              "before an input, -c:v names a decoder");
        Check(Used({"-i", "in.mov", "-c:v", "ffv1", "-c:a", "pcm_f32le", "o"})
                  .empty(),
              "an encoder outside the table is nobody's business");
        Check(
            Used({"-i", "in.mov", "-vf", "drawtext=text=a", "out.mp4"}).empty(),
            "LG6: drawtext is no longer asked of FFmpeg");
        Check(
            Names(UsedWithFilter({"-i", "in.mov", "-vf",
                                  "scale=320:-2,drawtext=textfile='/tmp/a.txt'",
                                  "out.mp4"})) == "drawtext",
            "a filter in a -vf chain");
        const std::string labelled =
            "[0:v]scale=1920:1080[base];\n"
            "[base] drawtext@title=fontsize=40[v]";
        Check(Names(UsedWithFilter({"-i", "a", "-i", "b", "-filter_complex",
                                    labelled, "-map", "[v]", "-c:v",
                                    "hevc_videotoolbox", "o"})) ==
                  "drawtext,hevc_videotoolbox",
              "after a label, with an instance name, and the encoder too");
        Check(
            Names(UsedWithFilter(
                {"-f", "lavfi", "-i", "color=c=black,drawtext=text=a",
                 "-frames:v", "1", "-f", "rawvideo", "pipe:1"})) == "drawtext",
            "a lavfi input is a graph");
        Check(UsedWithFilter(
                  {"-f", "rawvideo", "-i", "drawtext=notagraph", "out"})
                  .empty(),
              "any other input is a file, whatever its name");
        const std::string lookalikes =
            "subtitles='/tmp/drawtext.srt',nodrawtext=1,"
            "drawtextual=2,lut3d=file='/x/drawtext=y.cube'";
        Check(UsedWithFilter({"-i", "in", "-vf", lookalikes, "out"}).empty(),
              "the name inside another word or a path is not the filter");
        Check(Used({}).empty() && Used({"-c:v"}).empty(),
              "an empty or truncated line uses nothing");
    });
}

void TestRefusal(const std::filesystem::path& root) {
    const std::filesystem::path log = root / "calls.log";
    const std::filesystem::path reduced = root / "reduced" / "ffmpeg";
    // No libx265: what a slim build looks like.
    WriteFakeFfmpeg(
        reduced, log, {},
        {"h264_videotoolbox", "hevc_videotoolbox", "libx264", "prores_ks"});

    Test("a launch that needs a missing encoder is refused by name", [&] {
        process::Request request;
        request.arguments = {reduced.string(), "-i",      "in.mov",
                             "-c:v",           "libx265", "out.mp4"};
        const process::Result result = ffmpeg::Run(request);
        Check(result.stage == process::Stage::kRefused && !result.Started(),
              "the launch is refused");
        const std::string& refusal = result.refusal;
        Check(ffmpeg::RefusalName(refusal) == "FfmpegCapabilityMissing" &&
                  ffmpeg::RefusalName("unable to export: " + refusal) ==
                      "FfmpegCapabilityMissing",
              "under its name, wrapped or not: " + refusal);
        Check(Contains(refusal, "has no 'libx265' encoder") &&
                  Contains(refusal, reduced.string()) &&
                  Contains(refusal, "version 7.1-test"),
              "it names the encoder, the binary and its version: " + refusal);
        Check(Contains(refusal, "HEVC export cannot use the software") &&
                  Contains(refusal, "--enable-libx265") &&
                  Contains(refusal, "CUTMACHINE_FFMPEG"),
              "what it prevents and what to do: " + refusal);
        Check(Lines(log) == kProbeCalls,
              "the binary was asked its three listings and nothing else");
    });

    Test("the binary is probed once", [&] {
        process::Request request;
        request.arguments = {reduced.string(), "-i",      "in.mov",
                             "-c:v",           "libx265", "out.mp4"};
        const process::Result result = ffmpeg::Run(request);
        Check(result.stage == process::Stage::kRefused &&
                  Contains(result.refusal, "has no 'libx265' encoder") &&
                  Contains(result.refusal, "--enable-libx265"),
              "refused again: " + result.refusal);
        Check(Lines(log) == kProbeCalls,
              "without asking the binary again, and without launching it");
    });

    Test("the engine's own tools refuse before they launch", [&] {
        std::string error;
        // An export on the software HEVC encoder this binary lacks: refused
        // at the door, before anything is rendered.
        ExportPlan plan;
        plan.settings.ffmpeg_path = reduced.string();
        plan.settings.output_path = (root / "film.mp4").string();
        plan.temporary_output_path = (root / "film.part.mp4").string();
        plan.arguments = {"-i",
                          "in.mov",
                          "-c:v",
                          "libx265",
                          "-c:a",
                          "aac",
                          plan.temporary_output_path};
        int progressed = 0;
        Check(!Exporter::Run(
                  plan, [&](const ExportProgress&) { ++progressed; }, nullptr,
                  error),
              "the export is refused");
        Check(ffmpeg::RefusalName(error) == "FfmpegCapabilityMissing" &&
                  Contains(error, "'libx265'") && progressed == 0 &&
                  !std::filesystem::exists(plan.settings.output_path),
              "by name, with the encoder, and nothing is written: " + error);

        // The same refusal when the encoder is a segment's, not the
        // assembly's.
        ExportPlan segmented;
        segmented.settings = plan.settings;
        segmented.arguments = {"-i", "list.txt", "-c:v", "libx264", "out.mp4"};
        segmented.segments.push_back(plan);
        Check(!Exporter::Run(segmented, {}, nullptr, error) &&
                  Contains(error, "'libx265'"),
              "a segment's encoder is checked with the plan: " + error);

        Check(Lines(log) == kProbeCalls, "and none of them launched FFmpeg");
    });

    Test("what the binary has is launched", [&] {
        process::Request request;
        request.arguments = {reduced.string(), "-i",      "in.mov",
                             "-c:v",           "libx264", "out.mp4"};
        Check(ffmpeg::Run(request).Succeeded(), "an encoder it has is run");
        std::vector<std::string> expected = kProbeCalls;
        expected.push_back("-i in.mov -c:v libx264 out.mp4");
        Check(Lines(log) == expected, "the launch is the fourth call");
    });

    Test("a line that uses nothing of the table is not probed for", [&] {
        const std::filesystem::path other = root / "other" / "ffmpeg";
        const std::filesystem::path otherLog = root / "other.log";
        WriteFakeFfmpeg(other, otherLog, {}, {});
        process::Request request;
        request.arguments = {other.string(), "-i",    "in.mov",
                             "-f",           "s16le", "pipe:1"};
        Check(ffmpeg::Run(request).Succeeded(), "a decode is launched");
        Check(Lines(otherLog) ==
                  std::vector<std::string>{"-i in.mov -f s16le pipe:1"},
              "as the only call the binary receives");
    });

    Test("a binary that changed is probed again", [&] {
        std::filesystem::remove(log);
        WriteFakeFfmpeg(reduced, log, {}, kEveryEncoder);
        std::string error;
        Check(ffmpeg::Require(reduced.string(),
                              {"-i", "in", "-c:v", "libx265", "out"}, error),
              "the encoder it now has is accepted: " + error);
        Check(Lines(log) == kProbeCalls, "after one new probe");
    });

    Test("a binary that cannot be probed is not refused", [&] {
        const std::filesystem::path broken = root / "broken" / "ffmpeg";
        const std::filesystem::path brokenLog = root / "broken.log";
        WriteFakeFfmpeg(broken, brokenLog, {}, {},
                        "echo 'dyld: library not loaded' >&2; exit 1");
        const ffmpeg::Probe probe = ffmpeg::ProbeBinary(broken.string());
        Check(!probe.answered && Contains(probe.failure, "-filters") &&
                  Contains(probe.failure, "dyld: library not loaded"),
              "the probe says how it failed: " + probe.failure);
        std::string error;
        Check(ffmpeg::Require(broken.string(),
                              {"-i", "in", "-c:v", "libx265", "out"}, error) &&
                  error.empty(),
              "unknown is not absent: the launch is left to FFmpeg");
        Check(ffmpeg::Require((root / "absent" / "ffmpeg").string(),
                              {"-i", "in", "-c:v", "libx265", "out"}, error),
              "nor is a binary that is not there");
    });
}

// The diagnosis of the resolved binary, on the engine and its two surfaces.
void TestDiagnosis(const std::filesystem::path& root) {
    const std::filesystem::path log = root / "diagnosis.log";
    const std::filesystem::path binary = root / "resolved" / "ffmpeg";
    const std::filesystem::path package = root / "Film.cutmachine-project";
    setenv("CUTMACHINE_FFMPEG", binary.c_str(), 1);

    const auto parse = [](const std::string& text) {
        Value value;
        std::string error;
        Check(Value::Parse(text, value, error), "the answer is JSON: " + text);
        return value;
    };
    const auto text = [](const Value& object, const std::string& key) {
        const Value* value = object.Find(key);
        return value != nullptr && value->IsString() ? value->AsString()
                                                     : std::string();
    };
    const auto openProject = [&](McpProjectBackend& backend) {
        const McpToolRegistry registry;
        Value arguments = Value::MakeObject();
        arguments.Set("path", Value::MakeString(package.string()));
        return registry.Call(backend, "open_project", arguments);
    };

    Test("what the resolved binary lacks is said, row by row", [&] {
        WriteFakeFfmpeg(
            binary, log, {},
            {"h264_videotoolbox", "hevc_videotoolbox", "libx264", "prores_ks"});
        const ffmpeg::Diagnosis diagnosis = ffmpeg::Diagnose();
        Check(diagnosis.location.Found() && diagnosis.probe.answered &&
                  !diagnosis.Complete(),
              "the binary is found, probed, and incomplete");
        Check(diagnosis.missing.size() == 1 &&
                  Names(diagnosis.missing) == "libx265",
              "one row is missing: " + Names(diagnosis.missing));
        Check(diagnosis.ErrorName() == "FfmpegCapabilityMissing" &&
                  Contains(diagnosis.Detail(), "lacks 1 of the 5") &&
                  Contains(diagnosis.Detail(), "encoder 'libx265'"),
              "the detail counts and names them: " + diagnosis.Detail());

        const Value report = ffmpeg::DiagnosisJson(diagnosis);
        Check(text(report, "path") == binary.string() &&
                  text(report, "origin") == "CUTMACHINE_FFMPEG" &&
                  text(report, "version") == "7.1-test",
              "the report says which binary: " + report.Dump());
        const Value* rows = report.Find("capabilities");
        Check(rows != nullptr && rows->IsArray() && rows->AsArray().size() == 5,
              "and every row of the table");
        if (rows != nullptr && rows->IsArray()) {
            for (const Value& row : rows->AsArray()) {
                const std::string name = text(row, "name");
                const bool missing = name == "libx265";
                const Value* available = row.Find("available");
                Check(available != nullptr &&
                          available->Dump() == (missing ? "false" : "true"),
                      name + " is " + (missing ? "missing" : "available"));
                Check(text(row, "prevents").empty() == !missing &&
                          text(row, "build_option").empty() == !missing,
                      name + " says what it prevents only when missing");
            }
        }
    });

    // DEC32 -- an FFmpeg that is found exits 0, as `--check-setup` does;
    // what it lacks stays in the answer.
    Test("--check-ffmpeg answers with the diagnosis", [&] {
        std::string output;
        Check(CheckFfmpegCommand(output) == 0,
              "an incomplete FFmpeg exits 0: " + output);
        const Value answer = parse(output);
        const Value* ok = answer.Find("ok");
        const Value* complete = answer.Find("complete");
        Check(ok != nullptr && ok->Dump() == "true" && complete != nullptr &&
                  complete->Dump() == "false",
              "ok, and not complete: " + output);
        Check(text(answer, "error") == "FfmpegCapabilityMissing" &&
                  Contains(text(answer, "detail"), "encoder 'libx265'"),
              "the lack is named: " + output);
        const Value* missing = answer.Find("missing");
        Check(missing != nullptr && missing->IsArray() &&
                  missing->AsArray().size() == 1 &&
                  text(missing->AsArray()[0], "name") == "libx265" &&
                  Contains(text(missing->AsArray()[0], "prevents"), "HEVC"),
              "with what it prevents: " + output);
        Check(text(answer, "path") == binary.string() &&
                  answer.Find("capabilities") != nullptr &&
                  answer.Find("result") == nullptr,
              "and the whole report beside it, not under `result`: " + output);
    });

    Test("open_project and create_project say it as they start a session", [&] {
        McpProjectBackend backend("");
        const McpToolRegistry registry;
        Value arguments = Value::MakeObject();
        arguments.Set("path", Value::MakeString(package.string()));
        arguments.Set("name", Value::MakeString("Film"));
        const McpToolCallOutcome created =
            registry.Call(backend, "create_project", arguments);
        Check(created.ok, "the project is created: " + created.message);
        for (const McpToolCallOutcome& outcome :
             {created, openProject(backend)}) {
            Check(outcome.ok, "the session starts: " + outcome.message);
            const Value answer = parse(outcome.result_json);
            const Value* section = answer.Find("ffmpeg");
            Check(section != nullptr && section->IsObject(),
                  "the answer carries `ffmpeg`: " + outcome.result_json);
            if (section == nullptr) {
                continue;
            }
            Check(text(*section, "error") == "FfmpegCapabilityMissing" &&
                      text(*section, "path") == binary.string(),
                  "with the error and the binary");
            const Value* missing = section->Find("missing");
            Check(missing != nullptr && missing->IsArray() &&
                      missing->AsArray().size() == 1 &&
                      text(missing->AsArray()[0], "name") == "libx265" &&
                      text(missing->AsArray()[0], "kind") == "encoder" &&
                      Contains(text(missing->AsArray()[0], "prevents"), "HEVC"),
                  "and each lack with what it prevents: " + section->Dump());
            Check(!text(answer, "path").empty(),
                  "beside the answer it always gave");
        }
    });

    Test("a complete FFmpeg leaves the answers as they were", [&] {
        WriteFakeFfmpeg(binary, log, {"drawtext"}, kEveryEncoder);
        const ffmpeg::Diagnosis diagnosis = ffmpeg::Diagnose();
        Check(diagnosis.Complete() && diagnosis.ErrorName().empty() &&
                  diagnosis.Detail().empty(),
              "nothing is missing: " + diagnosis.Detail());

        std::string output;
        Check(CheckFfmpegCommand(output) == 0, "--check-ffmpeg exits 0");
        const Value answer = parse(output);
        const Value* ok = answer.Find("ok");
        const Value* complete = answer.Find("complete");
        Check(ok != nullptr && ok->Dump() == "true" && complete != nullptr &&
                  complete->Dump() == "true" &&
                  text(answer, "path") == binary.string() &&
                  text(answer, "version") == "7.1-test" &&
                  answer.Find("capabilities") != nullptr &&
                  !Contains(output, "prevents") &&
                  answer.Find("error") == nullptr &&
                  answer.Find("missing") == nullptr,
              "with the binary and its rows, all available: " + output);

        McpProjectBackend backend("");
        const McpToolCallOutcome outcome = openProject(backend);
        Check(
            outcome.ok && parse(outcome.result_json).Find("ffmpeg") == nullptr,
            "open_project says nothing of FFmpeg: " + outcome.result_json);
    });

    Test("no FFmpeg at all is the diagnosis too", [&] {
        setenv("CUTMACHINE_FFMPEG", (root / "nowhere" / "ffmpeg").c_str(), 1);
        const ffmpeg::Diagnosis diagnosis = ffmpeg::Diagnose();
        Check(!diagnosis.location.Found() &&
                  diagnosis.ErrorName() == "FfmpegNotFound" &&
                  Contains(diagnosis.Detail(), "FFmpeg not found: "),
              "the resolver's refusal is the detail: " + diagnosis.Detail());

        std::string output;
        Check(CheckFfmpegCommand(output) == 1, "--check-ffmpeg exits 1");
        const Value answer = parse(output);
        Check(text(answer, "error") == "FfmpegNotFound" &&
                  answer.Find("result") == nullptr,
              "named, with no report to give: " + output);

        McpProjectBackend backend("");
        const McpToolCallOutcome outcome = openProject(backend);
        const Value opened = parse(outcome.result_json);
        const Value* section = opened.Find("ffmpeg");
        Check(outcome.ok && section != nullptr &&
                  text(*section, "error") == "FfmpegNotFound" &&
                  Contains(text(*section, "detail"), "CUTMACHINE_FFMPEG") &&
                  section->Find("missing") == nullptr,
              "open_project still opens, and says so: " + outcome.result_json);

        // And a binary that is there but does not answer.
        const std::filesystem::path broken = root / "mute" / "ffmpeg";
        WriteFakeFfmpeg(broken, log, {}, {}, "exit 1");
        setenv("CUTMACHINE_FFMPEG", broken.c_str(), 1);
        // DEC32 -- found, so 0, as `--check-setup` counts it: incomplete.
        Check(CheckFfmpegCommand(output) == 0 &&
                  text(parse(output), "error") == "FfmpegProbeFailed" &&
                  parse(output).Find("complete") != nullptr &&
                  parse(output).Find("complete")->Dump() == "false" &&
                  Contains(output, broken.string()),
              "a binary that cannot be probed is named as such: " + output);
    });
}

}  // namespace

int main() {
    std::error_code ignored;
    const std::filesystem::path root =
        std::filesystem::canonical(std::filesystem::temp_directory_path()) /
        ("cutmachine-ffmpeg-capabilities-" + std::to_string(getpid()));
    std::filesystem::remove_all(root, ignored);
    std::filesystem::create_directories(root);
    // The machine's own settings must not decide this suite's answers.
    setenv("CUTMACHINE_ENV_FILE", (root / "settings.env").c_str(), 1);
    unsetenv("CUTMACHINE_FFMPEG");

    TestReading();
    TestRefusal(root);
    TestDiagnosis(root);

    std::filesystem::remove_all(root, ignored);
    if (failures != 0) {
        std::cerr << failures << " assertion(s) en échec\n";
        return 1;
    }
    std::cout << "All FFmpeg capability tests passed\n";
    return 0;
}
