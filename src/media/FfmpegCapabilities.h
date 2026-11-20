#pragma once

// PRD2 -- docs/roadmap/PRODUIT-2026-10.md. What the engine asks of FFmpeg
// that not every FFmpeg has, checked before it is asked.
//
// The engine encodes with libx264, libx265, VideoToolbox and prores_ks. A
// build without one of them used to fail in the middle of an export, in
// FFmpeg's words, after the motion frames were drawn and the segments
// rendered. The table also had a filter, drawtext, which Homebrew's build
// lacks: the captions no longer need it since LG3
// (docs/roadmap/LEGENDES-2026-10.md), which draws them with Core Text. The
// filter half of the machinery stays, for the next filter the engine needs.
//
// Three things live here:
//   - the table of those capabilities, each with what its absence prevents.
//     A capability the engine starts to depend on is one more row;
//   - the probe: the binary's own lists of filters and encoders, read once
//     per binary and kept;
//   - the check: which rows a command line uses, read from its arguments,
//     and the named refusal when the binary lacks one. ffmpeg::Run makes
//     that check on every launch, so no caller has to remember it; a caller
//     with work to do before the launch (Exporter::Run, before it draws
//     motion frames) makes it earlier, with Require.
//
// The check reads the command line rather than taking a declaration from
// each caller: the encoder is the argument after `-c:v`, and a filter of
// the table is looked for by name in the graphs the line carries. Only the
// rows of the table are ever looked for, so an unusual graph cannot be
// refused by mistake -- the worst a misreading does is leave the failure to
// FFmpeg, as before.
//
// A binary that cannot be probed is not a binary that lacks something: the
// check then lets the launch through, and FFmpeg says what is wrong.
//
// Plain C++ and POSIX: no AppKit.

#include "FfmpegBinary.h"
#include "Json.h"

#include <set>
#include <string>
#include <vector>

namespace ffmpeg {

// The stable names surfaces give the refusals below.
inline constexpr const char* kCapabilityMissingErrorName =
    "FfmpegCapabilityMissing";
inline constexpr const char* kProbeFailedErrorName = "FfmpegProbeFailed";
// How a capability refusal starts, for RefusalName (FfmpegBinary.h).
inline constexpr const char* kCapabilityMissingPrefix =
    "FFmpeg capability missing: ";
// The key an answer carries DiagnosisProblemJson under.
inline constexpr const char* kDiagnosisKey = "ffmpeg";

enum class CapabilityKind { kFilter, kEncoder };

// "filter" or "encoder".
const char* KindName(CapabilityKind kind);

struct Capability {
    CapabilityKind kind;
    const char* name;
    // What the engine cannot do without it, as a clause that follows
    // "Without it, ".
    const char* prevents;
    // The FFmpeg configure option a build needs for it.
    const char* build_option;
};

// Every capability the engine uses that an FFmpeg may lack.
const std::vector<Capability>& KnownCapabilities();

// What one binary says of itself.
struct Probe {
    std::string path;
    // The binary listed its filters and its encoders. False: `failure` says
    // what happened, and nothing below is known.
    bool answered = false;
    std::string failure;
    // "7.1" of "ffmpeg version 7.1 Copyright ..."; empty when the binary
    // does not introduce itself that way.
    std::string version;
    std::set<std::string> filters;
    std::set<std::string> encoders;

    bool Has(const Capability& capability) const;
};

// The three listings as FFmpeg prints them (`-version`, `-filters`,
// `-encoders`), read into a Probe. Split from the launches so the reading
// is tested on text.
Probe ParseProbe(const std::string& path, const std::string& versionOutput,
                 const std::string& filtersOutput,
                 const std::string& encodersOutput);

// Asks the binary at `path`, once: the answer is kept for the life of the
// process, for as long as the file there is the same one (size and
// modification time). A failed probe is not kept.
Probe ProbeBinary(const std::string& path);

// The rows of the table a command line uses. `arguments` is an FFmpeg
// command line, with or without the binary first.
std::vector<const Capability*> CapabilitiesUsed(
    const std::vector<std::string>& arguments);
// The same, against another table: a test reads a graph for a filter row
// the engine's table no longer has.
std::vector<const Capability*> CapabilitiesUsed(
    const std::vector<std::string>& arguments,
    const std::vector<Capability>& table);

// True when the binary a launch of `arguments` would run has everything
// they use. `requested` is the caller's `ffmpeg_path`: empty for the
// resolved binary. False with a named refusal -- FfmpegNotFound when there
// is no binary, FfmpegCapabilityMissing with the name of the filter or the
// encoder when it lacks one. Nothing is probed for a command line that
// uses no row of the table.
bool Require(const std::string& requested,
             const std::vector<std::string>& arguments, std::string& error);

// The resolved binary, what it says of itself, and what it lacks.
struct Diagnosis {
    Location location;
    // Meaningful when the location was found.
    Probe probe;
    std::vector<const Capability*> missing;

    // Found, probed, and lacking nothing.
    bool Complete() const {
        return location.Found() && probe.answered && missing.empty();
    }
    // FfmpegNotFound, FfmpegProbeFailed, FfmpegCapabilityMissing; empty when
    // Complete().
    std::string ErrorName() const;
    // One sentence for that error; empty when Complete().
    std::string Detail() const;
};

Diagnosis Diagnose();

// The whole diagnosis: path, origin, version, and every row of the table
// with `available` and what it prevents.
mcp_json::Value DiagnosisJson(const Diagnosis& diagnosis);

// Only what is wrong, for an answer that carries the diagnosis beside
// something else: the error and its detail, and the missing rows. Call it
// when !Complete().
mcp_json::Value DiagnosisProblemJson(const Diagnosis& diagnosis);

}  // namespace ffmpeg
