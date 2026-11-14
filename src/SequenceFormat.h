#pragma once

// SEQ-2026-08 -- deriving a sequence format from the rushes it will hold.
//
// The gap this closes is not the mutation: `update_sequence` has always been
// able to set width, height and rate. The gap is that somebody had to *know*
// the numbers, and the only caller who could was a human reading a media
// panel. An agent asked to "set the right format" would have to invent
// 2160x3840, which is exactly what PHILOSOPHY.md principle 7 forbids: the
// caller names the intent ("match the rushes"), the engine resolves the exact
// values. Same division of labour as ResolveIntervalSplits and
// ResolveWordRemoval.
//
// Two things make this worth an engine function rather than a glance at the
// library:
//
//   - Rotation. A phone or mirrorless rush is stored landscape with a 90°
//     display matrix; its *displayed* frame is portrait. Choosing the stored
//     size gives a sideways sequence, and no amount of care at the call site
//     prevents that mistake being made once per project. Ingest.cc computes
//     displayed size in floating point because it only needs the word
//     "portrait"; a sequence format needs exact pixels, so the swap here is
//     integer arithmetic on a right angle and nothing else.
//   - Disagreement. Real projects mix rates and sizes. The rule is majority,
//     with every other format reported alongside, so a caller can see what it
//     is not choosing instead of being silently conformed.

#include "Document.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct SequenceFormatCandidate {
    int32_t width = 0;
    int32_t height = 0;
    MediaRate frame_rate{0, 1};
    // How many media in the library display in exactly this format.
    size_t media_count = 0;
};

struct SequenceFormatProposal {
    SequenceFormatCandidate chosen;
    // Every distinct displayed format found, most common first, `chosen`
    // included. A caller that reports only `chosen` hides a mixed-rate
    // shoot from the person who needs to know about it.
    std::vector<SequenceFormatCandidate> candidates;
    // Media that carry a usable displayed format.
    size_t media_considered = 0;
    // Media left out: audio-only, zero-sized, no frame rate, or rotated by
    // something that is not a right angle. Never silently folded into a
    // candidate, because none of them has a format to contribute.
    size_t media_ignored = 0;
    bool unanimous = false;
};

// ITW5 -- ROADMAP.md. The colour input the rushes declare, read from the tags
// FFmpeg probed at ingest. The ITW-2026-09 rushes said arib-std-b67 in every
// file, and the project was set up as Rec.709 anyway, because nothing turned
// the tags into settings. What the tags cannot say -- S-Log3 carries no
// standard tag -- is reported as undetermined, never guessed.
struct ColorInputProposal {
    // "hlg", "rec709", a declared input such as "sony_slog3" (G3), or empty
    // when neither declarations nor tags decide.
    std::string input_transfer;
    std::string input_gamut;
    std::string input_ycbcr_matrix = "auto";
    std::string input_range = "auto";
    // Whether management is needed to show the rushes correctly: an HLG or
    // an S-Log3 input is; a Rec.709 one is not.
    bool enable = false;
    size_t media_considered = 0;
    size_t media_agreeing = 0;
    // Distinct declarations found, most common first, as
    // "transfer/primaries/matrix/range".
    std::vector<std::pair<std::string, size_t>> declarations;
};

// G3 -- docs/roadmap/GRADE-2026-09.md. Each media counts for the input it
// declares (LibraryMedia::color_input), or else for the one its tags name
// (ColorInputFromTags below); `declarations` still reports the tags.
bool ResolveColorInput(const std::vector<LibraryMedia>& library,
                       ColorInputProposal& proposal, std::string& error);

// G3 -- the input a video media's colour tags name: HLG with BT.2020
// primaries, or a BT.709-family transfer without them. Matrix and range stay
// "auto", which reads the file's own tags frame by frame. Undeclared (an
// empty transfer) for anything else: S-Log3, PQ, sRGB, untagged. Tags are
// not always the truth -- a Blackmagic Pocket 4K tags its Film log as
// BT.709 -- which is why ingest asks the camera's own metadata first
// (Ingest.cc).
MediaColorInput ColorInputFromTags(const LibraryMedia& media);

// Exact displayed dimensions, swapping width and height for a quarter turn.
// Returns false when `rotation_degrees` is not a multiple of 90, which no
// exact integer format can describe.
bool DisplayDimensions(const LibraryMedia& media, int32_t& width,
                       int32_t& height);

// Picks the format the most media already display in. Ties break on the
// larger frame, then the higher rate, then the wider frame -- deterministic
// down to the last field, so the same library always yields the same answer
// whatever order it is stored in. Fails only when no media has a usable
// format at all.
bool ResolveSequenceFormat(const std::vector<LibraryMedia>& library,
                           SequenceFormatProposal& proposal,
                           std::string& error);

// The proposal as the JSON both the CLI and the MCP tool report. Kept here so
// the two surfaces cannot drift into describing the same computation
// differently. `extraFields` is spliced in before the closing brace as
// already-serialized text (each starting with a comma), which is how the MCP
// tool adds what only it knows: whether it applied the format or found the
// sequence already conforming.
std::string SequenceFormatProposalJson(const SequenceFormatProposal& proposal,
                                       const std::string& extraFields = "");
