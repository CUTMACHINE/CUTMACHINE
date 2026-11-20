#pragma once

// The timeline's display list, built in portable C++. It was
// -timelineRenderData in main.mm: a thousand untested lines drawing the same
// rectangles TimelineView.cc makes clickable, so that the two could drift
// apart without any test failing (AGENTS.md, "Interface"). Here it is
// compiled under Linux with the model, and a test can compare what is drawn
// with what a press finds. The window gathers what it reads into a
// TimelineDrawingInput and draws what comes back; nothing in it decides a
// geometry any more.

#include "AgentJournal.h"
#include "Document.h"
#include "EffectsLibrary.h"
#include "Operations.h"
#include "Project.h"
#include "RenderData.h"
#include "TimelineView.h"
#include "TrimSession.h"
#include "UiTheme.h"
#include "Waveform.h"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

// FC4 -- docs/roadmap/FILMCRAFT-2026-10.md. The fifth tool was Slip; it is
// Resolve's Trim Edit mode now (TimelineView.h, TimelinePointerTool).
enum class TimelineTool { Select, Hand, Zoom, Cut, Trim };

// What the Record monitor's layers show at the playhead, slot by slot.
struct ResolvedSlot {
    bool active = false;
    Ulid sourceId;
    int64_t frame = -1;
    float opacity = 1.0f;
    // The DocumentClip currently occupying this slot. Used only to look up
    // that clip's color.* effects stack for F1.3 grading; see
    // presentNearestFrameAtDeadline.
    Ulid clipId;
};

// D6 -- docs/roadmap/DIALOGUE-2026-10.md. A marker being stretched into a
// span with Option held: the end that stays, and the operation the pointer
// has reached so far. Nothing is in the document until the button is
// released.
struct TimelineMarkerStretch {
    Ulid marker_id;
    RationalTime anchor;
    std::optional<UpdateMarkerOperation> reached;
};

// Out of lot -- the author, 18 November 2026 (GitHub #330). A fade handle of an
// audio clip being dragged: the press, and the settings the pointer has
// reached so far (TimelineView.h, UpdateTimelineFadeDrag). Nothing is in the
// document until the button is released.
struct TimelineFadeDragState {
    TimelineFadeDrag press;
    std::optional<SetClipAudioOperation> reached;
};

// Everything the timeline's drawing reads, as the window holds it. The
// pointers are the window's own state, read for the duration of the call
// and never kept; every one is required.
struct TimelineDrawingInput {
    const Document* document = nullptr;
    // The document as a trim in progress shows it (-trimShownDocument).
    const Document* shown_document = nullptr;
    const Project* project = nullptr;
    const TimelineViewport* viewport = nullptr;
    const TimelineInteraction* interaction = nullptr;
    const std::vector<ResolvedSlot>* requested = nullptr;
    const std::set<Ulid>* offline_source_ids = nullptr;
    const std::map<Ulid, AudioWaveform>* waveforms = nullptr;
    const trim::TrimSession* trim_session = nullptr;
    const AgentJournal* agent_journal = nullptr;
    const std::vector<Ulid>* retime_clip_ids = nullptr;

    // The drawable's width and the two panes' heights, in points.
    double width = 0.0;
    double timeline_height = 0.0;
    double video_height = 0.0;
    // The widest graduation label, measured in the atlas's face.
    double ruler_label_width = 0.0;
    // The width a marker's note takes, measured the same way.
    std::function<double(const std::string&)> marker_note_width;
    // The timeline surface has the keyboard.
    bool timeline_has_focus = false;
    bool has_valid_timeline_range = false;
    TimelineTool effective_tool = TimelineTool::Select;
    MediaRate playhead_rate{25, 1};

    RationalTime duration{0, 1};
    RationalTime requested_position{0, 1};
    std::optional<RationalTime> timeline_in;
    std::optional<RationalTime> timeline_out;
    int playback_direction = 0;
    std::optional<TimelineZoomBarDrag> zoom_bar_drag;
    bool linked_selection = true;
    bool lasso_dragging = false;
    double lasso_start_x = 0.0;
    double lasso_start_y = 0.0;
    double lasso_current_x = 0.0;
    double lasso_current_y = 0.0;
    std::optional<double> cut_preview_x;
    std::optional<double> cut_preview_y;
    int hovered_timeline_tool = -1;
    int hovered_track_control = -1;
    Ulid hovered_track_id;
    Ulid hovered_marker_id;
    Ulid hovered_clip_id;
    std::optional<TimelineMarkerStretch> marker_stretch;
    std::optional<TimelineFadeDragState> fade_drag;
    std::optional<ui::effects::DropTarget> effect_drop_target;
    // PRD24 -- the clip badges and the hidden angle's veil. The window
    // test builds the list once more without them, to hold that a clip
    // carrying nothing draws the commands it drew before PRD24. A field, set
    // through the test's hooks (AppTestHooks.h): this file is compiled once,
    // in the model, which knows nothing of the test.
    bool clip_signage = true;
};

// The timeline's display list, and the background of the Record monitor
// above it, for `input`.
TimelineRenderData BuildTimelineRenderData(const TimelineDrawingInput& input);

// A clip's body colour: its track kind's, lifted by a hash of its source so
// that neighbouring clips of different rushes read apart.
std::array<float, 3> ClipColor(const Ulid& sourceId, bool audio);
// A rush's name as the browser shows it: its display name, else its file's.
std::string RushDisplayName(const Project& project, const LibraryMedia& media);
// A rush's name on a timeline clip: as above, without its extension.
std::string TimelineClipName(const Document& document, const Project& project,
                             const Ulid& sourceId);
// PRD27 -- docs/roadmap/PRODUIT-2026-10b.md. The name a clip carries on the
// timeline: a motion clip its composition's (MOTION-2026-09 M4), a carton its
// words, any other its rush's. Drawn here and read by VoiceOver
// (-timelineAccessibilityValue): one rule, so that what is heard is what is
// seen. `sourceId` is the source the timeline resolved for the clip.
std::string TimelineDrawnClipName(const Document& document,
                                  const Project& project,
                                  const DocumentClip* clip,
                                  const Ulid& sourceId);
// D2 -- docs/roadmap/DIALOGUE-2026-10.md. A marker's colour is document
// data, `#rrggbb` by default; the document accepts any short text there, so
// what an agent wrote that is not a colour falls back on the theme's.
ui::theme::Color MarkerDisplayColor(const DocumentMarker& marker);
// A linked clip's drift from its sync anchor: in frames when it is a whole
// number of them, else in samples at 48 kHz.
std::string SyncDriftLabel(const RationalTime& drift, MediaRate frameRate);
