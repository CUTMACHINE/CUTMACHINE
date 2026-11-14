#include "FfmpegCapabilities.h"

#include "ProcessLauncher.h"

#include <sys/stat.h>

#include <chrono>
#include <map>
#include <mutex>
#include <sstream>
#include <utility>

namespace ffmpeg {
namespace {

using mcp_json::Value;

// A probe is three listings of a few kilobytes each. A binary that has not
// answered in this long is not going to.
constexpr std::chrono::seconds kProbeDeadline(10);

std::vector<std::string> Words(const std::string& line) {
    std::vector<std::string> words;
    std::istringstream input(line);
    std::string word;
    while (input >> word) {
        words.push_back(word);
    }
    return words;
}

// ` T.C drawtext          V->V       Draw text on top of video ...`
// The legend above the list has the same indentation; a filter's third
// word is its inputs and outputs, which always holds "->".
std::set<std::string> ParseFilters(const std::string& output) {
    std::set<std::string> filters;
    std::istringstream input(output);
    std::string line;
    while (std::getline(input, line)) {
        const std::vector<std::string> words = Words(line);
        if (words.size() >= 3 && words[2].find("->") != std::string::npos) {
            filters.insert(words[1]);
        }
    }
    return filters;
}

// ` V....D libx264              libx264 H.264 / AVC / MPEG-4 AVC ...`
// The legend's lines read ` V..... = Video`: their second word is "=".
std::set<std::string> ParseEncoders(const std::string& output) {
    std::set<std::string> encoders;
    std::istringstream input(output);
    std::string line;
    while (std::getline(input, line)) {
        const std::vector<std::string> words = Words(line);
        if (words.size() >= 2 && words[1] != "=" &&
            words[0].find_first_not_of("VASFXBD.") == std::string::npos) {
            encoders.insert(words[1]);
        }
    }
    return encoders;
}

std::string ParseVersion(const std::string& output) {
    const std::vector<std::string> words =
        Words(output.substr(0, output.find('\n')));
    // "<program> version <number> ...": the program is whatever the binary
    // is called.
    if (words.size() >= 3 && words[1] == "version") {
        return words[2];
    }
    return std::string();
}

// One listing: the binary with one option, its standard output whole.
bool Listing(const std::string& path, const std::string& option,
             std::string& output, std::string& failure) {
    output.clear();
    const auto deadline = std::chrono::steady_clock::now() + kProbeDeadline;
    process::Request request;
    request.arguments = {path, "-hide_banner", option};
    request.standard_output = process::Stream::kPipe;
    request.standard_error = process::Stream::kPipe;
    request.read_bytes = 65536;
    request.error_tail_bytes = 1024;
    request.on_output = [&output](const char* bytes, size_t count) {
        output.append(bytes, count);
    };
    request.cancelled = [deadline]() {
        return std::chrono::steady_clock::now() > deadline;
    };
    const process::Result result = process::Run(request);
    if (!result.Started()) {
        failure = "unable to start '" + path + "': " + result.SystemErrorText();
        return false;
    }
    if (result.cancelled) {
        failure = "'" + path + " " + option + "' did not answer";
        return false;
    }
    if (!result.Succeeded()) {
        failure = "'" + path + " " + option + "' failed" +
                  (result.error_tail.empty() ? std::string()
                                             : ": " + result.error_tail);
        return false;
    }
    return true;
}

Probe RunProbe(const std::string& path) {
    std::string version;
    std::string filters;
    std::string encoders;
    std::string failure;
    // The version is a courtesy: a binary that lists its filters and its
    // encoders has answered, whatever it calls itself.
    Listing(path, "-version", version, failure);
    failure.clear();
    if (!Listing(path, "-filters", filters, failure) ||
        !Listing(path, "-encoders", encoders, failure)) {
        Probe failed;
        failed.path = path;
        failed.failure = failure;
        return failed;
    }
    return ParseProbe(path, version, filters, encoders);
}

bool IsCodecOption(const std::string& argument) {
    return argument == "-c" || argument == "-codec" || argument == "-vcodec" ||
           argument == "-acodec" || argument.rfind("-c:", 0) == 0 ||
           argument.rfind("-codec:", 0) == 0;
}

bool IsGraphOption(const std::string& argument) {
    return argument == "-vf" || argument == "-af" ||
           argument == "-filter_complex" || argument == "-lavfi" ||
           argument == "-filter" || argument.rfind("-filter:", 0) == 0;
}

// Whether `graph` runs the filter `name`: the name where a filter starts
// (the start of the graph, or after `,`, `;` or a `]` label) and ends (at
// `=`, `@`, a separator, a label, or the end of the graph).
bool GraphUses(const std::string& graph, const std::string& name) {
    size_t at = 0;
    while ((at = graph.find(name, at)) != std::string::npos) {
        size_t before = at;
        while (before > 0 &&
               (graph[before - 1] == ' ' || graph[before - 1] == '\n')) {
            --before;
        }
        const bool starts = before == 0 || graph[before - 1] == ',' ||
                            graph[before - 1] == ';' ||
                            graph[before - 1] == ']';
        const size_t after = at + name.size();
        const bool ends =
            after == graph.size() ||
            std::string("=@,;[ \n").find(graph[after]) != std::string::npos;
        if (starts && ends) {
            return true;
        }
        at = after;
    }
    return false;
}

std::string MissingSentence(const Probe& probe, const Capability& capability) {
    return std::string(kCapabilityMissingPrefix) + "'" + probe.path + "'" +
           (probe.version.empty() ? std::string()
                                  : " (version " + probe.version + ")") +
           " has no '" + capability.name + "' " + KindName(capability.kind) +
           ". Without it, " + capability.prevents +
           ". Install an FFmpeg built with " + capability.build_option +
           ", or set " + kBinaryVariable + " to one that is";
}

Value CapabilityJson(const Capability& capability) {
    Value row = Value::MakeObject();
    row.Set("kind", Value::MakeString(KindName(capability.kind)));
    row.Set("name", Value::MakeString(capability.name));
    return row;
}

}  // namespace

const char* KindName(CapabilityKind kind) {
    return kind == CapabilityKind::kFilter ? "filter" : "encoder";
}

const std::vector<Capability>& KnownCapabilities() {
    static const std::vector<Capability> kCapabilities = {
        {CapabilityKind::kEncoder, "h264_videotoolbox",
         "an H.264 export cannot use the hardware encoder, the default of "
         "export_timeline and of render_motion_video",
         "--enable-videotoolbox"},
        {CapabilityKind::kEncoder, "hevc_videotoolbox",
         "an HEVC export cannot use the hardware encoder, the default of the "
         "window's presets and of export_timeline with codec: hevc",
         "--enable-videotoolbox"},
        {CapabilityKind::kEncoder, "libx264",
         "an H.264 export cannot use the software encoder (software: true)",
         "--enable-libx264"},
        {CapabilityKind::kEncoder, "libx265",
         "an HEVC export cannot use the software encoder (software: true, "
         "and the window's master preset)",
         "--enable-libx265"},
        {CapabilityKind::kEncoder, "prores_ks",
         "ProRes proxies cannot be generated, and render_motion_video cannot "
         "write a video with alpha",
         "--enable-encoder=prores_ks"},
    };
    return kCapabilities;
}

bool Probe::Has(const Capability& capability) const {
    const std::set<std::string>& list =
        capability.kind == CapabilityKind::kFilter ? filters : encoders;
    return list.count(capability.name) != 0;
}

Probe ParseProbe(const std::string& path, const std::string& versionOutput,
                 const std::string& filtersOutput,
                 const std::string& encodersOutput) {
    Probe probe;
    probe.path = path;
    probe.version = ParseVersion(versionOutput);
    probe.filters = ParseFilters(filtersOutput);
    probe.encoders = ParseEncoders(encodersOutput);
    // A listing that parses to nothing is a format this reader does not
    // know, not an FFmpeg without a single filter.
    probe.answered = !probe.filters.empty() && !probe.encoders.empty();
    if (!probe.answered) {
        probe.failure = "'" + path + "' listed no " +
                        (probe.filters.empty() ? "filter" : "encoder") +
                        " this engine could read";
    }
    return probe;
}

Probe ProbeBinary(const std::string& path) {
    struct Kept {
        off_t size;
        struct timespec modified;
        Probe probe;
    };
    static std::mutex mutex;
    static std::map<std::string, Kept> kept;

    struct stat status {};
    const bool present = stat(path.c_str(), &status) == 0;
#if defined(__APPLE__)
    const struct timespec modified = status.st_mtimespec;
#else
    const struct timespec modified = status.st_mtim;
#endif
    // Held across the probe: two threads that need it at once get one
    // probe, and the second waits for the first.
    const std::lock_guard<std::mutex> lock(mutex);
    const auto found = kept.find(path);
    if (present && found != kept.end() &&
        found->second.size == status.st_size &&
        found->second.modified.tv_sec == modified.tv_sec &&
        found->second.modified.tv_nsec == modified.tv_nsec) {
        return found->second.probe;
    }
    Probe probe = RunProbe(path);
    if (present && probe.answered) {
        kept[path] = {status.st_size, modified, probe};
    } else {
        kept.erase(path);
    }
    return probe;
}

std::vector<const Capability*> CapabilitiesUsed(
    const std::vector<std::string>& arguments) {
    return CapabilitiesUsed(arguments, KnownCapabilities());
}

std::vector<const Capability*> CapabilitiesUsed(
    const std::vector<std::string>& arguments,
    const std::vector<Capability>& table) {
    // An option before an input describes that input: `-c:v` there names a
    // decoder. Only what follows the last input is about what is written.
    size_t lastInput = 0;
    bool anyInput = false;
    for (size_t index = 0; index < arguments.size(); ++index) {
        if (arguments[index] == "-i") {
            lastInput = index;
            anyInput = true;
        }
    }
    std::set<std::string> encoders;
    std::vector<const std::string*> graphs;
    std::string inputFormat;
    for (size_t index = 0; index + 1 < arguments.size(); ++index) {
        const std::string& argument = arguments[index];
        const std::string& value = arguments[index + 1];
        if (IsCodecOption(argument) && (!anyInput || index > lastInput)) {
            encoders.insert(value);
        } else if (IsGraphOption(argument)) {
            graphs.push_back(&value);
        } else if (argument == "-f") {
            inputFormat = value;
        } else if (argument == "-i") {
            // `-f lavfi -i <graph>`: the input is a filter graph.
            if (inputFormat == "lavfi") {
                graphs.push_back(&value);
            }
            inputFormat.clear();
        }
    }
    std::vector<const Capability*> used;
    for (const Capability& capability : table) {
        bool uses = false;
        if (capability.kind == CapabilityKind::kEncoder) {
            uses = encoders.count(capability.name) != 0;
        } else {
            for (const std::string* graph : graphs) {
                uses = uses || GraphUses(*graph, capability.name);
            }
        }
        if (uses) {
            used.push_back(&capability);
        }
    }
    return used;
}

bool Require(const std::string& requested,
             const std::vector<std::string>& arguments, std::string& error) {
    const std::vector<const Capability*> used = CapabilitiesUsed(arguments);
    if (used.empty()) {
        return true;
    }
    std::string path;
    if (!Executable(requested, path, error)) {
        return false;
    }
    const Probe probe = ProbeBinary(path);
    if (!probe.answered) {
        return true;
    }
    for (const Capability* capability : used) {
        if (!probe.Has(*capability)) {
            error = MissingSentence(probe, *capability);
            return false;
        }
    }
    return true;
}

std::string Diagnosis::ErrorName() const {
    if (!location.Found()) {
        return kNotFoundErrorName;
    }
    if (!probe.answered) {
        return kProbeFailedErrorName;
    }
    return missing.empty() ? std::string() : kCapabilityMissingErrorName;
}

std::string Diagnosis::Detail() const {
    if (!location.Found()) {
        return location.refusal;
    }
    if (!probe.answered) {
        return "FFmpeg could not be probed: " + probe.failure;
    }
    if (missing.empty()) {
        return std::string();
    }
    std::string names;
    for (const Capability* capability : missing) {
        names += (names.empty() ? "" : ", ") +
                 std::string(KindName(capability->kind)) + " '" +
                 capability->name + "'";
    }
    return std::string(kCapabilityMissingPrefix) + "'" + location.path + "'" +
           (probe.version.empty() ? std::string()
                                  : " (version " + probe.version + ")") +
           " lacks " + std::to_string(missing.size()) + " of the " +
           std::to_string(KnownCapabilities().size()) +
           " capabilities the engine uses: " + names;
}

Diagnosis Diagnose() {
    Diagnosis diagnosis;
    diagnosis.location = Resolved();
    if (!diagnosis.location.Found()) {
        return diagnosis;
    }
    diagnosis.probe = ProbeBinary(diagnosis.location.path);
    if (!diagnosis.probe.answered) {
        return diagnosis;
    }
    for (const Capability& capability : KnownCapabilities()) {
        if (!diagnosis.probe.Has(capability)) {
            diagnosis.missing.push_back(&capability);
        }
    }
    return diagnosis;
}

Value DiagnosisJson(const Diagnosis& diagnosis) {
    Value root = Value::MakeObject();
    if (!diagnosis.location.Found()) {
        return root;
    }
    root.Set("path", Value::MakeString(diagnosis.location.path));
    root.Set("origin",
             Value::MakeString(OriginName(diagnosis.location.origin)));
    if (!diagnosis.probe.answered) {
        return root;
    }
    root.Set("version", Value::MakeString(diagnosis.probe.version));
    Value rows = Value::MakeArray();
    for (const Capability& capability : KnownCapabilities()) {
        Value row = CapabilityJson(capability);
        const bool available = diagnosis.probe.Has(capability);
        row.Set("available", Value::MakeBool(available));
        if (!available) {
            row.Set("prevents", Value::MakeString(capability.prevents));
            row.Set("build_option", Value::MakeString(capability.build_option));
        }
        rows.Push(std::move(row));
    }
    root.Set("capabilities", std::move(rows));
    return root;
}

Value DiagnosisProblemJson(const Diagnosis& diagnosis) {
    Value root = Value::MakeObject();
    root.Set("error", Value::MakeString(diagnosis.ErrorName()));
    root.Set("detail", Value::MakeString(diagnosis.Detail()));
    if (diagnosis.location.Found()) {
        root.Set("path", Value::MakeString(diagnosis.location.path));
    }
    if (!diagnosis.missing.empty()) {
        Value rows = Value::MakeArray();
        for (const Capability* capability : diagnosis.missing) {
            Value row = CapabilityJson(*capability);
            row.Set("prevents", Value::MakeString(capability->prevents));
            rows.Push(std::move(row));
        }
        root.Set("missing", std::move(rows));
    }
    return root;
}

}  // namespace ffmpeg
