# Editing API

The shared CLI/MCP tools `edit_source_range` and `delete_timeline_range`
expose Source/Record range editing without opening the monitors. Insert or
overwrite takes explicit Source and Record points; the code deduces the
missing point of a three-point edit. Delete keeps the empty range, or closes
it with `ripple:true`, respecting targeted tracks, locking and sync lock.
`preview:true` validates without writing and returns the canonical operation;
applying the edit creates a single undoable journal entry.

With `ripple:true`, `followers` says how the untargeted sync-locked tracks
follow. `cut` (the default) removes the same range from them: a cutaway that
straddles it loses frames in the middle. `continuous` applies the same time
change to them while keeping the frames of each clip continuous from its
head: a cutaway placed over removed words stays whole and ends with them.
`remove_words`, `clean_disfluencies`, `tighten_pauses` and
`remove_source_ranges` likewise carry along, by default, all unlocked
sync-locked tracks; `sync_track_ids` gives an explicit list.

An A/V pair cut by words keeps a single link group across all its pieces.
`ripple_trim` therefore no longer trims "the whole group": on each other
track, it takes the member whose same edge is closest. This is what makes it
possible to lengthen the tail of the last piece without lengthening the
others.

A cut, trim or deletion by words renews the identifier of the pieces it
creates. The delta returned by each tool names these identities: the clips
created carry `created: true` and the ones gone are listed in
`removed_clip_ids`; caption styles, markers, transitions and bins have their
`created_<type>_ids` and `removed_<type>_ids`, and `--apply-project-op`
returns `created_timeline_ids` and `removed_timeline_ids`. The caller does not
have to reread the project to find what it has just created. A clip in the
delta also carries its `speed` (`{"num":2,"den":1}`) when it is not 1/1 or
when the edit changed it, a return to 1/1 included: absent, it is 1/1 and has
not moved. This way an agent that keeps its model through deltas does not hold
on to the old speed after a `rate_stretch` or a `set_clip_speed`, and does not
deduce a wrong source range from it. `speed_ramp` is `true` as long as the
clip follows a ramp (its keys are in `describe`), `false` when the edit
removed it. `enabled` is `false` as long as the clip is disabled, `true` when
the edit re-enabled it; absent, the clip is active and has not changed.
Every journaled editing tool that changed the timeline returns a `delta`, even
when its backend gives none: the catalogue then computes it from the two
states of the timeline (PRD40). A preview, a proposal or a folded intervention,
which change nothing, have none; `apply_edit_plan` keeps its
`delta_as_prepared`.

`Operations.h` exposes `InsertClipOperation`, `RemoveClipOperation`,
`TrimClipOperation`, `MoveClipOperation`, `DeleteGapOperation` and
`SplitClipOperation`, as well as `RippleTrimOperation`, `RollEditOperation`,
`SlipEditOperation`, the linked variants of move, trim and remove, and
`SetClipAudioOperation` (exact dB gain and `RationalTime` fades),
`AddTrackOperation` for multitrack. Project markers are persistent
addressable objects; `AddMarkerOperation`, `RemoveMarkerOperation` and
`UpdateMarkerOperation` go through the same journal and appear in
`--describe` under the aliases `K1`, `K2`, etc. Tracks have the aliases `T1`,
`T2`… (their rank in `describe`); the clips of the first track are `A1`,
`A2`…, those of the second `B1`… Adding a track at a lower index, or removing
one, renumbers the following ones; a twentieth track has no alias (the clips
of the twentieth track are already called `T1`, `T2`…). The tools accept
these aliases, `--apply-op` does not: it takes the operation as is. The
sequence settings also go through `UpdateSequenceOperation`, with atomic
validation and undo/redo without replacing its ULID or its timeline;
`update_sequence` changes only the fields given (`name`, `width`, `height`,
`frame_rate`) and keeps the others. The behavioural roadmap is described in
[`docs/NLE_TIMELINE_SPEC.md`](../NLE_TIMELINE_SPEC.md).
`JoinClipOperation` is the exact persisted inverse of a cut. `EditLog::Apply`,
`Undo` and `Redo` return a named `EditError` and guarantee the document's
atomicity.

The log keeps, in the Insert/Remove inverses, the exact representations of the
`timeline_in` values affected by the ripple. This metadata is necessary to
restore the original canonical bytes when different timebases represent the
same instant. It is not an additional operation.

Two foundations deliberately remain independent of the interface:
`ProjectRecovery` writes and inspects a derived canonical autosave without
ever implicitly replacing the main document, and `KeyframeTrack` provides
keyframes with a stable ULID and Hold, Linear or CubicBezier interpolation.
Keyframe time stays rational until the interpolation fraction is computed;
only parameter values are floating-point.

## The other editing gestures

Beyond `edit_source_range` and `delete_timeline_range`, each editing gesture
has its tool, and each is a reversible operation of `Operations.h`:

- placing and removing: `insert_clip` (pushes the following clips of the
  track), `paste_clips` (a batch of clips, linked to each other or not,
  inserted or, with `overwrite: true`, laid over what is underneath without
  shifting anything), `remove_clip` (closes the gap, and the named
  synchronised tracks), `clear_clip` and `clear_clips` (leave the gap),
  `delete_gap` (removes a gap of exact duration and shifts what follows, on
  the track and on `linked_track_ids`);
- moving and trimming: `move_clip` (overwrites what it lands on),
  `move_clips` (at least two clips, an exact destination each, a single undo
  step), `trim_clip` (one edge, the clip alone), `ripple_trim`, `roll_edit`,
  `slip_edit`, `slide_edit` and `rate_stretch` (next section);
- cutting: `split_clip` (strictly inside the clip), `split_at_interval` (every
  N seconds in one call: the interval is given as a duration and the engine
  converts it to sequence frames; linked partners are cut in the same places);
- linking and unlinking: `detach_audio` (the sound of a video clip onto an
  audio track, linked in sync), `set_clip_link` (links two clips as a pair, or
  undoes the link), `split_link_groups` (separates merged links, see below).

A link brings together a picture and its sounds where they are together: the
members of a group overlap in time (a picture and its sound, a J or an L cut,
the sound of a recorder next to that of the camera). A project cut by an
earlier version (`remove_source_ranges`, `tighten_pauses`) may have all the
pieces of a block under a single link (#213): one click takes the whole block,
and a drag or a trim carries all its pieces along. `split_link_groups` gives
each position its own link, in one undo step (two beyond 256 links): the first
keeps the old one, each member is linked to the picture of its position, sync
offset included, and a clip alone at its position loses its link. A position
that a link refuses (a freeze, a ramp) keeps the old link. `preview: true`
counts without changing anything. The window applies this repair when it opens
a timeline, and says so in the status line (`LinkGroupRepair.h`).

A linked picture/sound group is handled in a single gesture:
`move_linked_clips` and `trim_linked_clips` (one destination or edge per
member), `shorten_linked_clip` ("remove N frames or seconds": the engine
resolves each member, the edge direction and the exact time; `preview: true`
validates without writing), `split_linked_clips` (all members at the same
instant, two new groups), `remove_linked_clips` and `clear_linked_clips`.

## Slip, slide and rate stretch

Slide and rate stretch are two finishing gestures taken from FilmCraft
(FILMCRAFT-2026-10 FC3), each an operation of `Operations.h`, an MCP tool and,
at the CLI, `--apply-op` with the whole operation. They are in the window
([`timeline-graphique.md`](timeline-graphique.md), "Trim mode and Trim
Monitor").

**`slip_edit`** (`SlipEditOperation`, type `SlipEdit`) slides the source range
under a clip by `delta`, without moving the clip or changing its duration. The
MCP tool takes the linked sound along like `slide_edit` (DEC40): without
`linked_clip_ids`, each named clip slips with its partner on each other track
(`SlipClipsWithLinkedPartners`, the slide's rule), and picture and sound stay
on the same frames. `[]` keeps the re-timing of one track against the other.
`SlipEditOperation`, at `--apply-op`, takes the list as it is.

```json
{"clip_ids":["A2"],"delta":{"value":2,"rate":25},"linked_clip_ids":[]}
```

**`slide_edit`** (`SlideEditOperation`, type `SlideEdit`) moves a clip by
`delta` between the two clips that touch it on its track. The clip keeps its
source range and its duration; the left neighbour lengthens (or shortens) by
its tail, the right one shortens (or lengthens) by its head, by the same
`delta`. Nothing else moves on the track, and the programme's duration stays
the same. Several named clips must share a link group; each slides between its
own neighbours.

Through the MCP tool, the linked sound follows, as for `ripple_trim`: without
`linked_clip_ids`, each named clip takes its partner along on each other track
(the member of its group whose head is closest, the rule of
`LinkedEdgePartnerIds`). An array that is present, even empty, is the caller's
exact choice: `[]` slides the picture alone, out of sync with its sound.
`SlideEditOperation`, at `--apply-op`, takes the list as it is.

```json
{"clip_ids":["A2"],"delta":{"value":3,"rate":25}}
```

Refused, by name: a clip without a neighbour touching it on one side or the
other (a gap is not a neighbour; `move_clip` moves a clip into a gap), a
neighbour that would fall to zero duration, a neighbour that has no source
frames left to lengthen into, a clip named twice or a neighbour shared by two
slides, a locked track. The checks are those of `roll_edit`: the right
neighbour reads its source clip at its speed, and a head that would fall in
the middle of a source frame of a slowed clip is refused; a transition placed
on one of the two joins follows the join, and is refused if the new handles no
longer feed it. On a caption track, a card slides between two cards without
reading any source.

**`rate_stretch`** (`RateStretchOperation`, type `RateStretch`) gives a clip a
new `duration` on the timeline while keeping the source range it reads: its
speed becomes range / duration, an exact reduced fraction (ten frames
stretched over twenty: `1/2`; over fifteen: `2/3`). `edge` says which edge
moves: `Tail` (the default) keeps the head in place, `Head` keeps the tail.
The clip's whole link group follows, as for `set_clip_speed`: picture and
sound keep a single speed, and the sound is stretched with pitch kept.

```json
{"clip_id":"A2","duration":{"value":20,"rate":25},"edge":"Tail"}
```

By default, nothing shifts: a stretch that bites into the next clip is refused
(`Overlap`), a shortened clip leaves a gap, like a `trim_clip`.

With `ripple: true` (DEC40), the stretch ripples, as in Resolve's Trim mode
(manual 21.1, p. 1219): each member keeps its head where it is, whatever
`edge` is (like a head `ripple_trim`), and any clip that starts at the
original end of a member, or after it, on its track, shifts by the difference
in duration. The other tracks that follow are `sync_track_ids`, as for
`ripple_trim`: without this field, the MCP tool takes each synchronised track
that carries no member, and names in `warning_track_ids` an unsynchronised
track that has clips after the clip; `[]` shifts only the group's tracks. On a
followed track, the added time opens at the original end of the named clip,
the removed time is the range [new end, old end), closed as by `ripple_trim`
(`CollapseClipThroughRange`). `sync_track_ids` without `ripple` is refused,
and so is a group that has two members on one track, with `ripple`: the
ripple of one would shift the other. A locked followed track stops the stretch
(`LockedTrack`). The two fields are written to the journal only when set: a
stretch without ripple keeps its bytes.

```json
{"clip_id":"A2","duration":{"value":20,"rate":25},"ripple":true}
```

Also refused, by name: a speed whose reduced fraction has a term beyond 1000
(ten frames over 1001: `10/1001`), never rounded; a clip with a speed ramp, a
freeze frame, a caption card, a motion clip; a group whose members do not have
the duration and speed of the named clip (a group cut into pieces by words,
for example: `detach_audio` or `set_clip_link` first); a head that would fall
before the start of the timeline; a locked track under one of the members.
The rules of `set_clip_speed` then apply: `source_in` on a source frame, and
no dissolve on a clip whose speed is not 1. The animation keys of a stretched
clip stay at their local time: they are not scaled.

Both operations keep in the journal the exact state of the tracks touched: an
undo returns the document byte for byte.

## Clip settings

What a clip carries beyond its boundaries is set tool by tool, each through a
reversible operation (`SetClipSpeedOperation`, `SetClipFramingOperation`,
`SetClipAnimationOperation`, `SetClipOpacityOperation`,
`SetClipEnabledOperation`, `SetClipBlurOperation`). They have no command of
their own: at the CLI, they go through `--call-tool`, or through `--apply-op`
with the whole operation. Every value is an exact fraction `{num, den}`, never
a float.

- **Speed** (`set_clip_speed`, REEL-2026-09 R3): a constant speed, `1/2` for a
  half-speed slow motion, `2` for twice as fast. The clip keeps its duration
  on the timeline and reads duration × speed of its source clip, without
  interpolating frames; on a linked clip, the whole group follows, and the
  sound is stretched with pitch kept. Refused when the clip's `source_in` does
  not fall on a frame of the source.
- **Speed ramp** (`set_clip_speed` with `speed_ramp`, REEL-2026-09 R3,
  criterion 4): keys `{time, value, interpolation}` in the clip's local time
  (0 is its first frame), each `linear` (the default) or `hold` until the
  next; the speed of the first key holds before it, that of the last after.
  The clip reads the integral of the curve, exact, without interpolating
  frames: `1` → `1/4` over 10 frames then back to `1` is a slow motion placed
  on one moment. At least two keys; a `cubic-bezier` key is refused (its
  integral is not exact: a soft ramp is written as closely spaced linear
  keys). Picture only: refused on an audio track and on a linked clip
  (`detach_audio` first). `speed` and `speed_ramp` exclude each other; setting
  one clears the other. A key identifier left out is generated. `describe`
  returns `speed_ramp` in the same form. The way back to Resolve (XML) refuses
  a ramped clip.
- **Freeze frame** (REEL-2026-09 R4): the speed `0/1`. The clip holds the frame
  where its `source_in` falls for its whole duration, and reads no range of its
  source clip; the monitor, the export, the sheets and `verify_export` all
  name this frame. `insert_clip` places it with `speed: {"num":0,"den":1}`
  (the `source_in` brought back to the start of its frame), including on the
  very last frame of a source clip, which an insertion at speed 1 would refuse;
  `set_clip_speed` freezes an existing clip. Picture only: refused on an audio
  track, and on a clip linked to a sound, which would have nothing to hold. The
  way back to Resolve (XML) still refuses it. At the CLI, through `--apply-op`
  (`InsertClip` with `speed`). A still image (PNG, JPEG, TIFF: a logo, a
  packshot) is ingested as a one-frame medium (R4): placed at speed 0/1, it
  holds the desired duration, its alpha kept, and lengthens or shortens like
  any other clip. A logo is declared as graphics (`set_media_color_input`,
  `graphics: true`, see [Sequence](sequence.md)). The `logo` piece remains for
  an animated logo.
- **Framing** (`set_clip_framing`, R1): `fit` (the default) keeps the whole
  image, `fill` covers the frame and cuts what overflows (a 16:9 source clip
  in a 1080 × 1920 sequence, without bars). `scale` multiplies the mode's
  scale, `position_x` and `position_y` move the centre by a fraction of the
  frame (x to the right, y downward), the four `crop_*` remove a fraction of
  each edge of the source; applied in the order rotation, crop, mode, scale,
  position. It is a complete framing: what is not given returns to its
  default. `list_clip_framing` returns, for an instant of a clip, the source
  region read, the rectangle the image occupies, the frame edges left
  uncovered in pixels (`uncovered`) and `covers_frame`; the monitor, the
  export, the sheets and `verify_export` place the clip on this same
  rectangle. In the window, the Composition section of the Inspector
  (Inspecteur) writes the same operation: **Cadrage** (Framing: Ajuster / Fit
  or Remplir / Fill), **Zoom** (from 1/8 to 8, to the thousandth),
  **Position X** and **Position Y** in sequence pixels from the centre, Y
  counted upward as in Resolve (the inverse of `position_y`). The Record
  monitor follows the cursor while it is dragged; the operation is written on
  release, one per gesture, and a double-click resets to 1 or 0. An animated
  zoom or position shows "animé" (animated) and stays on its animation.
- **Animation** (`set_clip_animation`, R2): keys on `opacity`,
  `framing.scale`, `framing.position_x` and `framing.position_y`, at least one
  per track (a single key holds its value over the whole clip, R2c), each at a
  time local to the clip (zero is its first frame), with `hold`, `linear` or
  `cubic-bezier` (four control points as exact fractions). The call gives the
  whole animation: what it does not give is removed, and an empty list removes
  all the keys. The keys stay on their frames when the clip is moved, trimmed
  or cut. On a motion clip, only opacity animates: an animated framing is
  refused at placement, and the export renders the fade frame by frame, with or
  without colour management (R2b). In the window, the Inspector's diamond sets
  or removes the key of the frame under the playhead, and an animated slider
  sets the one of the frame where it is released (R2c): these are the same
  operations. The opacity a clip shows is that of its keys, which replaces its
  constant, in the monitor as in the export.
- **Subject-following framing** (`track_subject_framing`,
  CADRAGE-2026-10): puts each clip in `fill`
  (scale and crops kept, or `scale` given) and sets linear keys on
  `framing.position_x` and `framing.position_y` that follow a face like a
  camera operator. The frame stays still as long as the face stays within
  `dead_zone_x`/`dead_zone_y` of its place (`target_x`, `target_y`); when it
  leaves, a movement bounded in speed (`max_speed`) and in acceleration
  (`max_acceleration`) brings it back to its place (to within `settle`) and
  stops, without going back on an overshoot that the zone still holds. A single
  subject: the face closest to `hint`, otherwise the largest, then the closest
  to its last place (within `gate` face widths). A detection gap of at most
  `bridge_gap` is bridged, a longer gap holds the frame. The frame never
  uncovers the edge: a subject out of reach is held at the edge and counted
  (`frames_at_edge`). Faces are read every `every_frames` frames by the app's
  detector (Vision, macOS only; refused by name elsewhere), or given in
  `detections` (time local to the clip, rectangles as fractions of the whole
  source image). The clip's other keys are kept; a motion clip or one whose
  scale is animated is refused. The result is a `SetClipFraming` and a
  `SetClipAnimation` in a batch: one undo step, replayed without a detector.
  The response gives, per axis, the keys, the movements, the margin and the
  frames held at the edge. At the CLI, through `--call-tool`.
- **Clip stabilisation** (`stabilize_clip`,
  PARITE-2026-11 PA7): reads in the source clip
  the displacement of the image between each frame of the clip and the next,
  smooths the camera path (a Gaussian average of standard deviation
  `smoothing`, 1/2 s by default), and writes the difference as linear keys on
  `framing.position_x` and `framing.position_y`. A pan stays, the shake around
  it goes away. The clip is put in `fill` and enlarged by the smallest scale
  that holds all the corrections, up to `max_scale` (5/4 by default); beyond
  that, the corrections stop at the edge of the margin and are counted
  (`frames_at_edge`) rather than showing a border. A `scale` given is taken as
  is. The clip's own position remains the centre around which the corrections
  move; its position keys are replaced, so the tool goes before
  `track_subject_framing`, not after. The response gives the scale chosen
  (`scale`, `scale_chosen`, `scale_needed`), the shake in sequence pixels
  before and after (`shake.before_rms_px`, `shake.left_rms_px`), the frames not
  measured (`frames_unmeasured`) and, per axis, the keys and the frames held at
  the edge. The keys follow the path to within 1/5000 of the frame
  (`tolerance`), that is, a key on almost every frame. What the tool does not
  do:
  - only translation is corrected: neither roll, nor zoom between two frames
    (measured and returned under `zoom_not_corrected_max_percent`), nor rolling
    shutter;
  - the measurement is that of the image, not of the camera: a subject that
    fills the frame and moves is read as a camera movement;
  - a clip that has a speed and a source clip that is not at the sequence's
    frame rate are refused; so is a motion clip or one whose scale is
    animated;
  - nothing has been seen on a film. On a synthetic 1280 × 720 source clip in
    which every displacement is known, the reading is right to 0.25 px root
    mean square (0.43 px at worst), and a 20 px shake is brought down to
    0.04 px for 4.4% of zoom.

  The result is a `SetClipFraming` and a `SetClipAnimation` in a batch: one
  undo step, replayed without decoding a frame. At the CLI, through
  `--call-tool`.
- **Opacity** (`set_clip_opacity`): the compositing opacity of a video clip.
- **Enable / disable** (`set_clip_enabled`, `clip_ids` and `enabled`):
  Resolve's Clip > Enable/Disable Clip, the D key of the window. A disabled
  clip stays on its track: it keeps its place, its duration, its links, its
  grade and its sound settings, edits move it and cut it like any other (its
  two halves stay disabled), and the timeline's duration counts it. It is
  neither seen nor heard: the monitor, playback sound, the export,
  `verify_export`, `transcribe_timeline`, `analyze_timeline_audio`, the
  timeline's transcript, the sheets, `timeline_stats`, `review_cuts` and the
  subtitles read it as a hole in its track (what is underneath, otherwise
  black and silence); in a dissolve, it comes out like a hidden multicam angle,
  the other side keeping its fade. It applies to any clip, picture, sound,
  motion or caption card; disabling a picture lets its linked sound play,
  which must be named too (the window takes them together when the linked
  selection is active). `enabled: true` returns the edit byte for byte;
  refused on a locked track. The file writes `"enabled": false` on the clip,
  in version 23 of the document; `describe` and the delta return it the same
  way, and `check_timeline` lists the disabled clips (`disabled`). On the way
  back to Resolve, the FCP7 XML writes the clip `<enabled>FALSE</enabled>` in
  its place (not yet verified in Resolve), the EDL counts it as a loss
  (`disabled_clips_not_sent`), and adding through `AppendToTimeline` refuses
  it by name. The rule is in `ClipEnabled.h`; a new reader of picture or sound
  asks for it.
- **Mask** (`set_clip_mask`,
  PARITE-2026-11 PA8): a shape that cuts out
  the image of a video clip. Inside, the clip; outside, the tracks underneath
  (`inverted` reverses). `shape` is `rectangle`, `ellipse` or `none`, which
  removes the mask. For a split screen, a picture in a circle, a window with
  soft edges.
  - The shape is placed **on the clip's image**, not on the frame:
    `center_x`, `center_y`, `width` and `height` are fractions of the
    displayed source clip (rotation applied, crops ignored). The mask
    therefore follows the clip when its framing moves or zooms, like a window
    in Resolve's Color page. An ellipse is the one inscribed in this
    rectangle.
  - `feather` softens the edge **inward**, from `0` (hard edge) to `1` (a fade
    from the edge to the centre): the size of the shape stays where the image
    ends.
  - The mask is a coverage that the colours do not see, multiplied by the
    opacity and the fades. It blends with the background like them: in the
    delivered signal without colour management, in linear light with it. Under
    colour management a softened edge is therefore lighter than in Resolve,
    which blends the coded values of its timeline
    ([measurement](../../tools/resolve-composite-probe/README.md)).
  - Every render evaluates the same rule (`ClipMask.h`): the export, the
    sheets and `read_frame` through an FFmpeg filter held to the rule within 2
    in 254 (`clip_mask_tests`), Record in its shader. Between Record and the
    file, the mask adds at most 0.017 to 0.026 of difference per pixel
    depending on the case, and 0.079 in managed S-Log3, where the display
    transform amplifies the file's 4:2:0 chrominance
    (`monitor_export_parity_tests`).
  - `read_frame` and the sheets show a clip alone: what the mask removes is
    black there.
  - Refused on an audio or subtitle clip and on a motion clip. `describe`
    writes it under `mask`, only on a masked clip; a document that carries one
    goes to version 26, which an older binary refuses instead of delivering the
    whole clip. `verify_export` counts the frames of a masked clip as not
    verifiable. EDL, OTIO, FCPXML and the send back to Resolve do not carry the
    mask and say so.
  - Not done: an animated mask, a free shape, a button in the Inspector,
    green-screen keying. Nothing has been seen on screen.
- **Blur** (`set_clip_blur`, EFFETS-2026-10 EF3): a Gaussian blur of a video
  clip's image, `blur` being its standard deviation (sigma) in sequence pixels,
  from `0` (none: the blur is removed) to `200`. In sequence pixels so that a
  clip blurs the same whatever the size of its source clip and its framing;
  each render carries it over into its own pixels (the export's resolution,
  Record's). The image is blurred in its own rectangle, edges extended, before
  its grade and its opacity: FFmpeg's `gblur` at export, the sheets and
  `read_frame` (`ClipBlur.h`), a Metal Performance Shaders Gaussian blur in
  Record; the difference between the two is measured and bounded by
  `monitor_export_parity_tests`. The fraction is reduced (`20/2` is stored as
  `10/1`). Refused on an audio or subtitle clip, on a motion clip (its
  composition blurs its own layers) and on a clip whose framing is animated (an
  animated opacity is accepted). `describe` writes it under `blur`, only on a
  blurred clip; a document that carries one goes to version 24.
  `verify_export` counts the frames of a blurred clip as not verifiable; the
  EDL and the Resolve XML do not carry it and say so (`blurs_not_sent`); the
  `AppendToTimeline` route refuses it by name. `measure_color`, `match_color`
  and `propose_grade` measure the clip before its blur; `read_frame` and the
  sheets show it blurred.

Transitions (`add_transition`, `update_transition`, `remove_transition`) are
of three types. On a video track, `cross_dissolve`, the default, spans the join
between two adjacent clips, and `dip_to_color` (R5) veils a junction with a
colour (`#rrggbb`, black by default); with no outgoing clip and with the
`StartAtCut` alignment, it opens the track, with no incoming clip and with
`EndAtCut`, it closes it. On an audio track, `cross_fade` (EF2, the default
type on this track) crosses two sounds: each is read beyond the join in its
handles, like a dissolve, one going down while the other goes up, at constant
power (Resolve's "Cross Fade +3 dB"). Playback, export and
`transcribe_timeline` hear it the same way. Each type is refused on the other
kind of track. `alignment` (`Center`, `StartAtCut`, `EndAtCut`) says how the
transition overlaps the join.

A timeline's look (`set_sequence_lut`, R6) is a `.cube` LUT applied once to
the whole sequence, after each clip's grade and before the output transform.
The file is copied into the package under its SHA-256, and the document keeps
only the fingerprint: the project renders the same image elsewhere. Under
colour management, the expected space is declared: `acescct` for a LUT
computed on the working space's log values, `rec709` for a LUT written for a
Rec.709 signal, refused if the project does not deliver SDR Rec.709; without
colour management, `space` is omitted. Called with no argument, the tool
removes the look. A 1D LUT, a size outside 2 to 65, a domain other than 0..1
or a value outside [0, 1] are refused by name.

## The catalogue by family

`cutmachine --tools` returns the catalogue, and it is the authority: each tool
has its description and its schema there. This table sorts it by family and
says where each family is described. Read from the binary, with CF1: 202
tools, `backup_project_archive`, `get_video_output` and `set_video_output`
included.

| Family | Tools | Described in |
|---|---|---|
| Project and timelines | `create_project`, `open_project`, `ingest_media`, `describe`, `list_timelines`, `add_timeline`, `clone_timeline`, `rename_timeline`, `remove_timeline` (never the last), `set_active_timeline`, `update_sequence`, `conform_sequence`, `create_timeline_variants`, `choose_timeline_variant` (the choice between timelines, in the project journal) | [Headless commands](commandes-headless.md), [Sequence](sequence.md), [Sequence format](format-de-sequence.md) |
| Tracks | `add_track`, `remove_track`, `set_track_lock`, `set_track_sync_lock`, `set_track_output` | this page |
| Clip editing | `insert_clip`, `edit_source_range`, `delete_timeline_range`, `paste_clips`, `remove_clip`, `clear_clip`, `clear_clips`, `trim_clip`, `move_clip`, `move_clips`, `split_clip`, `split_at_interval`, `delete_gap`, `detach_audio`, `ripple_trim`, `roll_edit`, `slip_edit`, `slide_edit`, `rate_stretch`, `set_clip_link`, `split_link_groups` | this page |
| Linked picture/sound groups | `move_linked_clips`, `trim_linked_clips`, `shorten_linked_clip`, `split_linked_clips`, `remove_linked_clips`, `clear_linked_clips` | this page |
| Clip settings, transitions, look | `set_clip_speed`, `set_clip_framing`, `list_clip_framing`, `set_clip_animation`, `track_subject_framing`, `stabilize_clip`, `set_clip_opacity`, `set_clip_blur`, `set_clip_mask`, `add_transition`, `update_transition`, `remove_transition`, `set_sequence_lut` | this page |
| A clip's sound | `set_clip_audio` | [Audio](audio.md) |
| Separate sound | `measure_audio_offset`, `sync_audio` | [Audio](audio.md) |
| Rendered sound | `render_sound`, `place_sound_stem` | [Audio](audio.md#rendering-a-stem-on-the-cuts) |
| Proposed ducking | `propose_ducking` | [Audio](audio.md#proposing-ducking-under-a-voice) |
| Transcription and words | `transcribe_media`, `transcribe_timeline`, `align_words`, `align_transcript`, `get_timeline_transcript`, `list_speech_blocks`, `inspect_speech_selection`, `list_source_words`, `list_timeline_words`, `locate_source_frame`, `analyze_speakers`, `list_speakers` | [Transcription](transcription.md) |
| Editing by speech | `create_interview_short`, `remove_words`, `remove_source_ranges`, `list_disfluencies`, `clean_disfluencies`, `analyze_hesitations`, `tighten_pauses`, `trim_boundary_air`, `close_junction_air`, `analyze_speech_onset`, `list_speech_onsets` | [Transcription](transcription.md) |
| Picture of the source clips | `analyze_shot_quality`, `list_shot_quality`, `list_stable_windows`, `analyze_isolated_frames`, `list_isolated_frames`, `timeline_stats` | [Quality control](controle-qualite.md), [Export](export.md) |
| Looking | `read_frame`, `contact_sheet`, `cut_sheet`, `media_sheet`, `inspect_cut`, `review_cuts` | [Looking](regarder.md) |
| Describing and searching shots | `describe_shots`, `search_shots` | [Describing shots](regarder.md#describing-shots-once-searching-them-afterwards) |
| Colour | `set_color_management`, `conform_color_management`, `set_media_color_input`, `set_media_white_balance`, `set_clip_effects`, `propose_exposure`, `propose_grade`, `measure_color`, `match_color`, `export_cdl`, `apply_cdl`, `paste_clip_grade` (one clip's grade pasted onto others, a single undo step) | [Measuring the grade](mesurer-etalonnage.md), [Sequence](sequence.md) |
| Music | `analyze_audio_beats`, `list_audio_beats`, `list_tempo_grid`, `list_music_structure`, `list_clip_tempo_beats` | [Export](export.md) |
| Edited sound | `analyze_timeline_audio` | [Export](export.md) |
| Captions | `add_caption_style`, `update_caption_style`, `remove_caption_style`, `set_clip_caption`, `generate_captions`, `translate_captions`, `train_caption_breaks`, `check_caption_legibility`, `import_srt` | [Export](export.md) |
| Markers and bins | `add_marker`, `update_marker`, `remove_marker`, `add_bin`, `rename_bin`, `move_bin`, `remove_bin` (empty only), `set_media_bin`, `set_timeline_bin` (a timeline, through the project journal) | this page, [Bins](chutiers.md) |
| Multicam | `add_multicam_group`, `set_multicam_active_angle`, `split_multicam_group`, `remove_multicam_group`, `list_multicam_groups` | [Headless commands](commandes-headless.md#multicam) |
| Motion | `list_motion_compositions`, `describe_motion_composition`, `add_motion_composition`, `edit_motion_composition`, `remove_motion_composition`, `humanize_motion_composition`, `list_motion_templates`, `list_fonts`, `add_motion_template`, `save_motion_template`, `render_motion_frame`, `render_motion_video`, `insert_motion_clip`, `place_motion_clip`, `set_clip_motion_props`, `check_motion_safe_area`, `export_motion_after`, `verify_after_export`, `import_after_comp` (an After composition read by the bridge, what is not translated goes in `notes`, MB7) | [`MOTION_FORMAT.md`](../MOTION_FORMAT.md) |
| After Effects | the fifteen `after_*` | [`AFTER_BRIDGE_MVP.md`](../AFTER_BRIDGE_MVP.md) |
| DaVinci Resolve | `send_to_resolve`, `import_resolve_timeline`, `export_resolve_xml` | [Resolve](resolve.md), [Export](export.md) |
| EDL CMX 3600 | `export_edl`, `export_edl_markers`, `import_edl_markers` | [EDL and WebVTT](edl.md) |
| OTIO and FCPXML | `export_otio`, `export_fcpxml` | [OTIO and FCPXML](echange.md) |
| Delivery | `check_timeline`, `export_timeline`, `export_timelines`, `verify_export` | [Export](export.md) |
| Journal, interventions, plans | `undo`, `redo`, `begin_intervention`, `end_intervention`, `prepare_edit_plan`, `get_edit_plan`, `apply_edit_plan`, `discard_edit_plan`, `list_changes`, `list_motifs` (and the `motif` argument of each edit, the project's included; `journal: "project"` rereads the project journal) | [Headless commands](commandes-headless.md#the-motive-of-an-edit) |
| Background tasks | `get_task`, `list_tasks`, `cancel_task` | [Headless commands](commandes-headless.md) |
| Durable steps and archive | `commit_run_step`, `get_run_step`, `archive_project`, `query_project_archive`, `project_archive_status`, `sync_project_archive`, `import_project_archive`, `backup_project_archive` (the day's copy of the archive, DEC44) | [Durable steps](orchestration-montage.md), [SQL archive](archive-projets.md) |
| The open app and know-how | `get_editor_view`, `capture_editor_window`, `get_video_output`, `set_video_output`, `get_procedure` | [Headless commands](commandes-headless.md) |
| Paid generation, on request | `propose_generation` (free), `generate_media` (spends, after the person's yes) | [Paid generation](generation.md) |
| Voice-over locally, on request | `design_voice` (a voice from a description), `generate_voice_over` (a text spoken by this voice); free, on the machine | [Generation, the voice-over](generation.md#the-voice-over-locally) |
