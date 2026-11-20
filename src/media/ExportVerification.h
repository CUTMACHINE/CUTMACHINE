#pragma once

#include "Document.h"
#include "IsolatedFrames.h"

#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

// ITW1 -- ROADMAP.md. Compares an exported file with the document it was
// rendered from, frame by frame, and names the frames that diverge.
//
// This exists because nothing else looks at the delivered file:
// contact_sheet and cut_sheet render from the document, so an export that
// showed the next source frame from a cut onwards, or held a cutaway one
// frame past its end, passed every check the agent had. The agent wrote this
// comparison itself (1 361 frames against their expected source frames, by
// normalized correlation) to find three of 18 visible shot starts shifted.
//
// The expected picture is resolved by Timeline -- the monitor's own rule,
// highest visible video track on top -- and each source is decoded
// sequentially onto its own frame grid, never through the input seek the
// exporter uses: a verification that shared the export's seek could not see
// that seek being wrong.
//
// It measures and decides nothing about the edit. Frames it cannot compare
// honestly (dissolves, translucent clips) are counted as unverifiable rather
// than guessed at, and a frame whose neighbours look identical (a locked-off
// shot) is not reported as shifted, because no picture evidence says so.
//
// REMOTION-PARITY-2026-09 REM6 -- a motion clip with nothing under it is
// checked too, against the composition frames the engine renders for it:
// a film made of motion alone no longer passes with nothing looked at. What
// that checks is the placement -- which composition frame the file shows at
// each sequence frame, fitted where -- not the drawing, which it shares with
// the export and which the motion corpus proves. A motion clip over another
// picture stays unverifiable: its alpha lets through a picture the
// thumbnails here do not composite.

struct ExportVerificationSettings {
    // Long edge of the luma thumbnails compared. Large enough to see a mouth
    // or a hand move between two frames, small enough to hold a whole edit.
    int32_t thumbnail_dimension = 64;
    // Source frames searched on each side of the expected one.
    int32_t search_radius = 2;
    // R8 -- docs/roadmap/REEL-2026-09.md. The isolated-frame pass runs on
    // the delivered file with the same rule it runs on the rushes, off the
    // thumbnails this verification already decodes: no second decode, and
    // the mean of an area-downscaled plane is the mean of the original.
    IsolatedFrameSettings isolated_frames;
    // Empty: the binary FfmpegBinary.h resolves (PRD1).
    std::string ffmpeg_path;
    // A one-frame offset no neighbour corroborates is looked at again, at
    // four times the thumbnail's size, over the frames around it only, and
    // reported only if that look agrees. At 64 px, on graded log rushes, two
    // neighbouring source frames can differ by more than the single-frame
    // margin, and the two one-frame offsets verify_export reported on a 4K
    // reel were both wrong: at 960 x 540 the delivered frame was the
    // expected one (25 September 2026).
    bool confirm_single_offsets = true;
    // When not empty, only these timeline frames are compared; the others
    // are counted unverifiable. What the confirmation pass runs on.
    std::set<int64_t> only_frames;
};

struct ExportDivergence {
    // "offset": the frames show another frame of the expected source;
    // "wrong_picture": they match no frame near the expected one;
    // "picture_in_gap": the document has no picture there, the file does;
    // "black_frame": the document has a picture there, the file is black.
    std::string kind;
    int64_t first_frame = 0;  // Export frames, inclusive.
    int64_t last_frame = 0;
    Ulid clip_id;
    Ulid source_id;
    int64_t expected_source_frame = 0;  // At first_frame.
    // Shown minus expected source frame, for kind "offset".
    int64_t offset = 0;
    // REM6 -- a motion clip's run: source_id is then its composition, and
    // the frames are the composition's, at its own cadence.
    bool motion = false;
};

struct ExportVerificationReport {
    Ulid timeline_id;
    MediaRate frame_rate{25, 1};
    int64_t expected_frames = 0;
    int64_t exported_frames = 0;
    int64_t checked_frames = 0;
    // REM6 -- of the checked frames, those a motion clip draws.
    int64_t motion_checked_frames = 0;
    int64_t unverifiable_frames = 0;
    // Checked frames whose neighbours were too alike to tell an offset.
    int64_t ambiguous_frames = 0;
    int64_t divergent_frames = 0;
    // One-frame offsets the closer look did not confirm: not divergences,
    // not verified either.
    int64_t unconfirmed_offset_frames = 0;
    std::vector<ExportDivergence> divergences;
    // R8 -- the frames of the delivered file that their neighbours do not
    // account for: a flash, a black frame, a corrupted one. Frame numbers
    // are export frames, like every other figure above.
    //
    // Measured on the file alone, so unlike everything else here it needs
    // no expectation to compare against -- which is why it covers the
    // frames the comparison counts unverifiable. That is deliberate and it
    // is the point: a one-frame flash inside a dissolve is exactly a frame
    // this verification cannot otherwise judge, and at half opacity it is
    // still the thing nobody wants delivered.
    //
    // Reported, never graded. The same measurement finds a mistake and a
    // figure of editing, and which one it is belongs to the author.
    std::vector<IsolatedFrameEvent> isolated_frames;
};

// Relative media paths resolve against projectDirectory, as in export.
bool VerifyExport(const Document& document,
                  const std::filesystem::path& projectDirectory,
                  const std::string& exportPath,
                  const ExportVerificationSettings& settings,
                  ExportVerificationReport& report, std::string& error);

// PRD38 -- docs/roadmap/PRODUIT-2026-10b.md. What "exact" guarantees: the
// file holds exactly the frames the timeline asks for, every one of them was
// compared with the picture expected there, and none diverges. A frame of a
// still shot whose neighbours look alike was compared and counts; a
// one-frame offset the closer look did not confirm is no divergence.
//
// It used to leave the unverifiable frames out: an export of 150 frames, 111
// of them under a motion clip over a picture, read "exact": true with 39
// frames looked at, and an agent reads the boolean, not the counts. A frame
// this check cannot compare -- a dissolve, a translucent, fading or blurred
// (EF3) clip, a motion clip over a picture, a colour dip on its ramp, a source
// that ends early -- now keeps it false, and the report says so beside it
// (verified_fraction, not_exact_because) rather than calling the file
// wrong: divergent_frames still says whether anything compared diverged.
bool ExportVerificationIsExact(const ExportVerificationReport& report);

std::string SerializeExportVerification(const ExportVerificationReport& report);
