#pragma once

// DEC15 (GR15) -- docs/roadmap/GRADE-RESOLVE-2026-10.md. What was graded in
// a DaVinci Resolve project, read clip by clip from a copy of its database,
// without Resolve.
//
// A reading, not an operation: nothing here touches a Document. What it
// returns travels in the answer of import_resolve_timeline, beside the cut
// it imports. GR16 -- a grade that color.resolve_primaries renders whole is
// written on its clip by the import (ResolveGradeStages, ResolveImport.h);
// for the others, the agent brings the picture back (the method of 15
// October: a LUT per clip exported by Resolve, curves of the engine fitted
// on it).
//
// The format is Resolve's own and undocumented. Where the grade lives and
// how a version body is laid out was measured on 20 October 2026 (GR15),
// after the format notes of samuelgursky/davinci-resolve-mcp (MIT, commit
// a07f594, resolve-advanced/vendor/drx-codec and drx-parameters): read, never
// run, no code taken. Its parameter identifiers are used as data, and only
// the primaries' are read -- everything else is refused *by name*, never
// skipped in silence: a plug-in, a window, a qualifier, a curve, a LUT, a
// keyframed or tracked setting, a node graph that is not a simple chain, a
// group's grade, a remote version.
//
// Values are the floats Resolve stores, never converted. What each one
// does was measured in Resolve 21.1 on 25 October 2026 (GR15), and
// color.resolve_primaries (ColorEffects.h) renders them as stored; Resolve's
// panel shows the lift halved, the gamma quartered and the offset as
// 25 + 25 times its value.
//
// Portable (label `portable`): SQLite and the zstd decoder are compiled into
// the engine on every host.

#include <cstdint>
#include <string>
#include <vector>

// One setting of a node's primaries, named after Resolve's panel ("lift.r",
// "contrast", "temperature"...), with the float the database holds.
struct ResolveGradeSetting {
    std::string name;
    float value = 0.0f;
};

struct ResolveGradeNode {
    // The number Resolve shows on the node.
    int64_t number = 0;
    std::string label;
    bool enabled = true;
    std::vector<ResolveGradeSetting> settings;
};

// What the reading saw and does not read, in French, where it sits: `node`
// is the node's number, 0 for the grade as a whole (its wiring, a group, a
// remote version, a block outside the nodes).
struct ResolveGradeUnread {
    int64_t node = 0;
    std::string what;
};

struct ResolveGrade {
    // The version's name in Resolve ("Version 1").
    std::string version;
    // In the order the grade applies them when `serial` is true; as stored
    // otherwise, and then the wiring is among `unread`.
    std::vector<ResolveGradeNode> nodes;
    bool serial = true;
    std::vector<ResolveGradeUnread> unread;
};

// The ceiling on one decompressed grade: the largest measured on the Mac
// (46 117 grades, 20 October 2026) is a few kilobytes; a frame announcing
// more than this is refused rather than allocated.
inline constexpr size_t kMaxResolveGradeBytes = 16u << 20;

// Decodes one ListMgt::LmVersion.Body: a byte 0x81 then a zstd frame, or
// 0x80 then the bytes themselves, then protobuf. False, with `error`, only
// when the body cannot be read at all (another first byte, a damaged frame,
// malformed protobuf); what is read but not understood is in
// `grade.unread`. `grade.version` is left as it was.
bool DecodeResolveGradeBody(const std::string& body, ResolveGrade& grade,
                            std::string& error);

// One video item of a timeline, as the database holds it.
struct ResolveDatabaseClip {
    // Resolve's own number: V1 is 1 (the first entry of the sequence's
    // VideoTrackVec).
    int32_t track = 0;
    // Timeline frames as stored, the timeline's origin included (90000 for
    // a 01:00:00:00 start at 25 i/s), half-open with `duration`.
    int64_t start = 0;
    int64_t duration = 0;
    // False when Start or Duration is not a whole number of frames: such an
    // item is never matched.
    bool whole_frames = true;
    std::string name;
    std::string path;
    // Something to say about this clip's grade: a correction in its active
    // version, or anything the reading cannot read (a body it cannot
    // decode, a graded remote version, a graded group).
    bool graded = false;
    ResolveGrade grade;
};

struct ResolveDatabaseTimeline {
    std::string name;
    std::vector<ResolveDatabaseClip> clips;
    // The timeline's own grade, applied after every clip's.
    bool graded = false;
    ResolveGrade grade;
};

// GR16 -- docs/roadmap/GRADE-RESOLVE-2026-10.md. The two project settings a
// node's contrast depends on, read from the project's own configuration
// (SM_Config.FieldsBlob): its field SetupBA, a 0x81 byte and a zstd frame
// of protobuf that names nothing. Which field holds which setting was found
// on 25 October 2026 by switching one setting at a time in a probe project
// and comparing: field 209 is the colour-space-aware tools (absent off, 1
// on), and the message of field 36 (past an eight-byte header) holds the
// contrast S-curve in its field 19 (0 off, absent on). Field 202 is the
// timeline's working luminance, found the same way on the same day: absent
// for HDR 1000, Resolve's default, 0 for SDR 100, 6 for HDR 2000, 8 for
// HDR 4000. It changes the contrast with the aware tools only -- by 134
// codes for a contrast of 1.2 at HDR 4000, 258 at SDR 100 -- and that
// contrast was measured at HDR 1000 alone. Field 7 is the colour science,
// found the same way on 6 November 2026: absent for DaVinci YRGB, 2 for
// DaVinci YRGB Color Managed v1, 6 for ACEScc, 8 for ACEScct, 10 for
// DaVinci YRGB Color Managed (today's, v2). A node renders the same in YRGB
// and Color Managed v2; in ACEScct it grades ACEScct codes, and its gamma,
// saturation and temperature and tint follow laws of their own there
// (measured on 7 and 14 November 2026, rendered with `acescct`,
// ColorEffects.h). `read` is false, with `unread` saying why, when the
// configuration could not be read: then no contrast, saturation,
// temperature or tint is rendered.
struct ResolveProjectSettings {
    bool read = false;
    bool color_space_aware = false;
    bool contrast_s_curve = true;
    // As Resolve's settings name it; "valeur <n>" for a value of field 202
    // not seen in the probe.
    std::string working_luminance = "HDR 1000";
    // As Resolve's settings name it; "valeur <n>" for a value of field 7
    // not seen in the probe.
    std::string color_science = "DaVinci YRGB";
    std::string unread = "réglages du projet non lus";
};

// GR16 -- the colour sciences each part of a node was measured in, by the
// name ResolveProjectSettings gives them: since 14 November 2026 the same
// three for both, DaVinci YRGB, Color Managed v2 and ACEScct (the contrast
// there with the colour-space-aware tools off only); never Color Managed v1
// or ACEScc.
bool ResolveScienceRendersPrimaries(const std::string& science);
bool ResolveScienceRendersBalance(const std::string& science);

// The settings from SM_Config's FieldsBlob as the database holds it.
// Never fails: what it cannot read leaves `read` false.
ResolveProjectSettings ParseResolveProjectSettings(const std::string& blob);

// Copies `databasePath` -- a Resolve Project.db, with its -wal and -shm
// beside it when they exist -- into a fresh temporary directory, reads every
// timeline named `timelineName` from the copy, then removes the copy. The
// database itself is never opened: Resolve may hold it, and a reader must
// not be able to change it. False, with `error` naming why, when the file
// cannot be copied or the copy is not a database this reading knows
// (another schema, a damaged copy). No timeline of that name is not an
// error: `timelines` comes back empty.
// GR16 -- `settings`, when given, receives the project's settings
// (ParseResolveProjectSettings) from the same copy.
bool ReadResolveDatabaseTimelines(
    const std::string& databasePath, const std::string& timelineName,
    std::vector<ResolveDatabaseTimeline>& timelines, std::string& error,
    ResolveProjectSettings* settings = nullptr);

// One grade as JSON: {"version":..,"serial":..,"nodes":[{"node":n,
// "label":..,"enabled":..,"settings":{"lift.r":-0.04,...}}],"unread":[
// {"node":n,"what":".."}]}. Floats in their shortest exact form.
std::string SerializeResolveGrade(const ResolveGrade& grade);
