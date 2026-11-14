#pragma once

// EF1 -- docs/roadmap/EFFETS-2026-10.md. The effects library: what the
// « Effets » tab lists, and where an effect dropped on the timeline lands.
//
// Asked by the author on 29 October 2026: effects that are dragged onto the
// timeline, as in Resolve's Effects Library. The tab draws the list and the
// timeline says where the pointer is; everything between -- which cut, which
// operation, how long, why not -- is decided here, in portable C++ the tests
// drive without a window. Nothing here mutates a document: the answer is an
// Operation the app hands to EditLog::Apply, the same one add_transition
// builds, so a dropped effect is one undo and replays from the journal.
//
// THE CUT UNDER THE POINTER. Inside a clip, the nearer of its two edges, as
// Resolve does with a transition dropped on a shot: the head when the
// pointer is in its first half, the tail otherwise. An edge whose
// neighbour does not touch it is the edge of the track there, which only a
// dip to a colour can take (an opening or closing fade).
//
// THE LENGTH. One second, Resolve's default, centred on the cut and
// shortened to what the media holds either side: a dissolve and a cross
// fade read their handles (TransitionRender.h), a dip only has to stay on
// the clips it covers. A cut that already has a transition gets the new
// type with its length and alignment kept, as Resolve replaces one.
//
// EF3 -- A CLIP EFFECT. The « Effets vidéo » land on the clip under the
// pointer, not on a cut: the whole clip is the target, as in Resolve's
// Effects Library. The Gaussian blur sets the clip's blur (SetClipBlur) to a
// default the Inspector then adjusts; dropped on a clip already blurred it
// is refused, rather than stacked or silently reset.

#include "Document.h"
#include "Operations.h"

#include <optional>
#include <string>
#include <vector>

namespace ui::effects {

enum class Category { VideoTransition, AudioTransition, VideoEffect };

struct Effect {
    // Stable and ASCII: what the pasteboard carries.
    const char* id;
    const char* title;
    Category category;
    // The document's type (TransitionRender.h); empty for a clip effect.
    const char* transition_type;
    // The colour a dip goes through; the default for every other type.
    const char* color;
    const char* help;
};

// EF3 -- whether the effect lands on a clip rather than on a cut.
bool IsClipEffect(const Effect& effect);

// EF3 -- the blur a dropped « Flou gaussien » gives a clip, in sequence
// pixels: a hundred-and-eighth of the frame's short side, which is 10 on a
// 1080p frame, horizontal or vertical, and 20 on a 2160p one. Strong enough
// to read at once as a blur on the monitor -- faces lose their features,
// shapes and colours stay, the background a title is laid over -- and light
// enough that the Inspector's « Flou » takes it from there either way. The
// short side, so that a 9:16 frame blurs as its 16:9 twin.
EffectParamValue DefaultClipBlur(const DocumentSequence& sequence);

// In the order the tab lists them.
const std::vector<Effect>& Catalog();
const Effect* FindEffect(const std::string& id);
const char* CategoryTitle(Category category);
// "video" or "audio": the kind of track the category's effects go on.
const char* CategoryTrackKind(Category category);

// Where an effect dropped at `time` on `trackId` lands.
struct DropTarget {
    bool valid = false;
    Ulid track_id;
    // Empty at the head of a track's run of clips, or at its tail.
    Ulid left_clip_id;
    Ulid right_clip_id;
    // Where the cut falls on the timeline.
    RationalTime cut{0, 1};
    // EF3 -- a clip effect's target: the clip under the pointer, the whole
    // of it. Empty for a transition, which lands on the cut above.
    Ulid clip_id;
    // In the interface's French, when !valid.
    std::string refusal;
};

DropTarget FindDropTarget(const Document& document, const Effect& effect,
                          const Ulid& trackId, const RationalTime& time);

// EF3 -- a clip effect aimed at one clip by its id, wherever it is: the
// selected clip, when the effect is double-clicked in the list.
DropTarget ClipEffectTarget(const Document& document, const Effect& effect,
                            const Ulid& clipId);

// The operation a drop applies, or why there is none. `summary` says what
// it did, for the status line.
struct DropEdit {
    std::optional<Operation> operation;
    std::string summary;
    std::string refusal;
};

DropEdit EditForDrop(const Document& document, const Effect& effect,
                     const DropTarget& target);

}  // namespace ui::effects
