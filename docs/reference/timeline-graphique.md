# Graphical timeline

> Mouse gestures and rendering are not verified on screen: the colours named
> here are those of the `src/UiTheme.h` tokens that `timelineRenderData` uses.
> The trim mode, the Trim Monitor, the slide, the rate stretch and the snap to
> In/Out points belong to FILMCRAFT-2026-10 (FC-window).

The timeline is drawn below the monitors, in its own `CAMetalLayer`. Tracks,
visible clips, gaps, selection border, trim preview, ruler and playhead are
Metal primitives; only the information strip under the surface is an AppKit
label. Hit-testing works on the rectangles computed by `TimelineViewport`, with
a fixed edge zone of 6 points.

An edge drag does not modify the document during the gesture. It builds a
preview, validates a `TrimClipOperation` on a copy, then emits at most one
operation through `EditLog::Apply` on release. An invalid preview is red and
emits nothing. `Cmd+Z` and `Cmd+Shift+Z` use the same journal as the CLI.
`Shift` during the drag enables ripple trim: the edge is trimmed and all
downstream clips of the sync-locked tracks follow the junction. `Cmd` enables
roll edit on two contiguous clips: the cut moves, but the end of the program
does not change. With the Trim tool (`T`), the upper part of a clip's body
slides its source content under a rectangle whose position and timeline
duration stay fixed (slip); the gesture is bounded by the start and end of the
source clip. The linked variants apply the same exact delta to the separate
picture and audio rectangles; each is journalled as a single undoable
operation.

Every trim stops where the first of its clips runs out, as Resolve's edge
stops at the last frame of the media: a simple trim at the source clip, at the
neighbour and at one frame of length; ripple and roll at the source clip and
at one frame, with no neighbour since they move what surrounds them; a linked
trim at the shortest of its members. A ripple carries only the linked clips of
its own group. With the Trim tool, the cursor tells the gesture before the
press: the double arrow on an edge (ripple, roll), the open hand on the upper
part of the body (slip), the pointing finger on its name bar (slide).
`Cmd+C` copies the selection and `Cmd+V` pastes it as an overwrite at the
playhead; `Option`-drag duplicates directly to the targeted position. A linked
selection recreates separate picture/audio clips with a new link group, in a
single undoable `PasteClipsOperation`.
The body of a clip can be moved horizontally or to another track of the same
type. `MoveClipOperation` keeps the ULID, the source times and the duration; an
incompatible destination refuses the drop. An overlap performs a non-ripple
overwrite: fully covered clips disappear, edge intersections are trimmed and a
clip that is crossed is split into two survivors. The exact states of the
affected tracks are journalled so that undo/redo restores the ULIDs and the
rational representations to the byte. Trims are bounded before display by the
start of the timeline, the source limits, a minimum duration of one tick and
the neighbouring clips: the handle can therefore never cross a valid limit.
Nearby junctions snap within a fixed zone of 8 points and show an orange
guide; `N` turns this magnetism on or off. It takes the edges of the clips of
all tracks, the markers, 0, and the timeline's In and Out points
(`SetInOutSnapTargets`, FC1), except the clips that the gesture carries along.
The selected clip is surrounded by a light border of fixed thickness,
independent of the zoom.

Hover (cursor, information strip) and click read the same target,
`TimelineHitAt` (`src/TimelineView.h`, FC2): zoom band, marker flag, clip body
or edge, trim mode, duplication, selection toggle. The context menu keeps its
own test.

With the Selection tool, a drag started in an empty area of the tracks draws
an orange lasso in the Metal surface. Past a threshold of 4 points, all clips
whose rectangle intersects the lasso are selected, including across several
tracks; their borders stay marked on release. Below the threshold, the gesture
remains a simple click that moves the playhead. A selection of unrelated clips
is moved and deleted as a block (`MoveClipsOperation`, `ClearClipsOperation`);
`Shift`+click on the body of a clip adds it to the selection or removes it.
`Cmd`+click does the same, as in Resolve (DEC40), with one nuance: `Cmd` held
from the press makes the playhead a snap target for the drag that follows, so
that a `Cmd`+click on an already selected clip removes it from the selection
only on release, if it has not moved anything; dragged, it takes the selection
to the playhead. `Option`+`Cmd` remains duplication, `Cmd` on an edge the
roll. For a linked A/V group, move, trim and delete each use an atomic
multi-clip operation so that a gesture cannot be partially applied.

A click in a bounded gap selects its exact range and shows it in orange.
`Delete` or `Backspace` then closes up the track by shifting all its following
clips to the left, without moving the other tracks. This gap closing is a
single `DeleteGapOperation` persisted in the event log; it is therefore
undoable and replayable to the byte. When **Sélection liée** (Linked
Selection) is on and the following clip has a linked audio, the gap closing
shifts the picture and audio tracks together by the same exact duration.

The palette at the top left exposes the Selection (`A`), Hand (`H`), Zoom
(`Ctrl+Z`, the Control key, with `Option` to zoom out), Blade (`B`) and Trim
(`T`) tools. The blade shows the future cut in orange and a click in a clip
creates two contiguous segments, with a new stable ULID for the right-hand one.
When **Sélection liée** is on, the same cut is applied atomically to the
separate audio clip and both sides become two new independent A/V pairs. The
selection also allows continuous scrubbing in the gaps and the ruler. `Espace`
starts or stops playback; held during a drag, it temporarily becomes the Hand
tool. `J`, `K` and `L` control reverse playback, stop and forward playback.
`Shift+Z` fits the whole timeline, `Cmd+=`/`Cmd+-` (or `+`/`-` alone) zoom
around the playhead, Home/End reach the ends, the left/right arrows advance by
one frame (`Shift`: one second of timecode), and up/down go to the
previous/next junction. Holding `Cmd` during the scrub snaps the playhead to
the nearest junction; during a move or a trim, `Cmd` also adds the playhead to
the snap targets.
A media dragged from the Médias (Media) panel is edited as in Resolve (manual
21.1, p. 891-893): as an overwrite, `Cmd+Shift` for an insert, `Option` for
picture only and `Shift` for sound only. The picture goes on the targeted video
track and the sound on the audio track of the same number (dropped on V2, the
sound goes to A2), or the reverse when the pointer is on an audio track. A
track is created where there is none to receive it: in the empty space below
the last track or on a caption track (one track of each kind that the media
carries), when the number is missing for the other kind, or when the partner
track is locked. A locked track under the pointer refuses what it should
receive. A media with no picture (music) is edited onto an audio track alone.
The drop, its created tracks included, is a single undo step
(`TimelineDropTracksAt`, `TimelineView.h`). There is no `+` line under the
headers: the Timeline menu, the timeline's context menu, `Cmd+Shift+T` (video
track) and `Cmd+Option+Shift+T` (audio track) add an empty track. Creation is
atomic and undoable; a clip can then be dragged to a compatible track with the
Selection tool. The player resolves and composites all the video tracks of the
document, with no functional limit fixed at two layers. `Cmd+T` adds a fade
centred on the cut at the playhead (or on a junction next to the selected
clip): a dissolve on a video track, a cross-fade on a sound track
(EFFETS-2026-10 EF2). The transition is a sequence object distinct from the
clips: it references the two clips, validates the exact media handles, is
journalled through `Add/Update/RemoveTransition` and is rendered identically in
Metal and in the FFmpeg export. It neither creates nor modifies any clip.
`D` (menu **Clip > Activer / désactiver le plan** (Clip > Enable / disable
clip), or right-click on a clip) is Resolve's Enable/Disable Clip: on the whole
selection, if at least one clip is active, all are disabled, otherwise all are
re-enabled, in one `SetClipEnabledOperation` and one undo step. A clip on a
locked track keeps its state. A disabled clip stays in place, greyed out (body
without tint, waveform and name dimmed, `kClipDisabled` in `UiTheme.h`), and is
neither seen nor heard in playback or export ([editing API](api-edition.md),
« Réglages d'un plan »). With linked selection, a click takes the picture and
its sound: `D` disables them together. This rendering has not been seen on
screen.

The **Effets** (Effects) tab of the left panel (EF1) lists the effects by
category: video transitions (dissolve, fade to black, fade to white), audio
transitions (+3 dB cross-fade) and video effects (Gaussian blur, EF3). A row
can be dragged onto the timeline. A transition targets the nearest junction of
the clip under the pointer, highlighted during the drag, and the drop places a
one-second transition, centred, shortened to the available handles, or changes
the type of the one already there; only fades to black and to white are placed
at the edge of a track. A video effect targets the whole clip under the
pointer, highlighted from edge to edge: the Gaussian blur gives it a blur of
one hundred and eighth of the short side of the picture (10 px on a 1080p
sequence, horizontal or vertical), which the **Flou** (Blur) row of the
Inspector (Composition section, 0 to 100 px) then adjusts; dropped again on a
clip that is already blurred, it is refused and refers to the Inspector. A
motion clip, a sound clip and a clip whose framing is animated take no blur;
the reason is shown in the status bar. A double-click in the list places a
transition at the junction nearest the playhead, a video effect on the
selected clip, or failing that on the topmost video clip under the playhead.
Each drop is a single operation, a single undo step.

A right-click on a track offers **Supprimer la piste** (Delete track) and
**Supprimer les pistes vides** (Delete empty tracks). An occupied track asks
for confirmation; its clips are included in the inverse of
`RemoveTrackOperation`, so that `Cmd+Z` restores the exact track and its
content.

The menu **CUTMACHINE > Raccourcis clavier…** (Keyboard shortcuts…) (`Cmd+,`)
allows reassigning the tools, J/K/L playback, the In/Out points, delete/ripple
delete, the dissolve, magnetism, linked selection, framing and the Source to
Record commands. A visual AZERTY keyboard lets one choose a command and its
modifiers, then click directly on the key to assign to it. Text entry remains
available for combinations such as `A`, `Space`, `F9`, `Alt+X` or
`Cmd+Shift+L`. An empty value disables the command. Duplicates and conflicts
with the fixed system shortcuts are refused; a button restores the default
values. The configuration is global to the application, persisted in the macOS
preferences, and the menus are refreshed immediately. Full screen is assigned
to `P` by default and can be reassigned in the same window.

**Affichage → Sortie vidéo (clean feed)** (View → Video output (clean feed))
shows the whole Record image on another HDMI/USB-C display, leaving the editor
usable. The editor's screen is greyed out; **Désactivée** (Off) stops the
output. Captions, motion and grading stay visible, while controls, scopes,
selections, zoom and reference comparison stay in Record. In trim, the output
shows the two images without their counters. It is the same Rec.709 SDR
preview as Record. The choice is a local preference; a disconnection suspends
the output and reconnecting restores it. The agent reaches it through
`get_video_output` and `set_video_output`.

The default keys follow those of DaVinci Resolve 21.1 ("DaVinci Resolve"
preset, read from its scripting API): Selection `A`, Blade `B`, Trim `T`, Fit
all `Shift+Z`, Insert `F9`, Overwrite `F10`. The Suppr. key is
`Shift+Delete`, the ripple delete, like `Del` in Resolve. Two commands that
Resolve does not have take a combination that it leaves free: the Zoom tool
(`Ctrl+Z`) and the frame/sample toggle of the playhead (`Alt+Shift+M`). The
table and its verification against Resolve are in `src/KeyboardShortcuts.h` and
`tests/keyboard_shortcuts_tests.cc`. A customization that is already saved
keeps its keys. The former Slip tool (`tool.slip`) became the Trim tool
(`tool.trim`); a custom key stored under the old name goes back to the Trim
tool as long as no key is stored under `tool.trim` (`RenamedCommands`,
`StoredSpecFor`), and is stored under the new name at the next « Enregistrer »
(Save) of the shortcuts panel.

A key whose character requires `Maj` (Shift) is read without that `Maj`: on a
French keyboard, the period is `Maj` and the `;` key, and it counts as the
period; `Maj+,` (`?` on AZERTY, `<` on QWERTY) counts as `Maj+,`. `Maj+.` cannot
be typed on AZERTY: the large step forward goes through the **Trim** menu or
another key there (`TypedKeyMatches`, `src/KeyboardShortcuts.h`).

## Trim mode and Trim Monitor

FILMCRAFT-2026-10. The policy is that of `src/TrimSession.h` (FC4); the window
reads its state from it and applies what it plans, one operation per gesture
via `EditLog::Apply`, hence a single undo step per gesture. The keys are those
of Resolve 21.1's default preset, with the manual page alongside:

| Key | Command | Resolve 21.1 manual |
|---|---|---|
| `T` | Trim tool: an edge ripples (`Cmd`: roll), the upper part of the body slides its content (slip), its name bar slides it between its neighbours (slide) | p. 1061, 1063-1066, 1069 |
| `A` | Selection tool: the same keyboard gestures resize instead of rippling | p. 1061-1062, 1069 |
| `V` | The edit point nearest the playhead, on the targeted tracks, as a roll; the playhead goes there | p. 1069 |
| `U` | Side of the point: out (A), roll, in (B), in turn | p. 1070 |
| `,` / `.` | One frame back / forward | p. 1069-1070 |
| `Maj+,` / `Maj+.` | Five frames (the "fast nudge", 5 by default) | p. 1069-1070 |
| `E` | The selected points to the playhead (Extend Edit) | p. 1070, 1088-1089 |
| `W` | Dynamic trim: `J` and `L` trim on the clock instead of playing, `K` writes the offset reached in a single operation, `Échap` abandons it without writing anything | p. 1069, 1085-1086 |
| `Cmd+R` | Speed controls of the selected clip; one end of their strip, dragged, stretches the clip (rate stretch), and ripples with the Trim tool | p. 1218-1219 |
| `Échap` | Abandons a dynamic trim, otherwise leaves trim mode, and closes the speed controls | p. 1222 |
| `/` | Plays from 2 s before to 2 s after the edit taken, the selected clips or the playhead, then returns the playhead there (ON9) | p. 1070 |
| `Cmd+/` | Loop playback: `/` starts over instead of stopping (ON9) | p. 1070 |

`V` takes a single instant, the nearest junction, on each targeted track that
has one (`SelectNearestEdit`): with all tracks targeted, taking the nearest of
each track would carry a distant music cut along.
A click on an edge that moves nothing selects its point (out for the end of a
clip, in for a start, roll with `Cmd`); `Maj`-click adds it to the selection or
removes it. The points are marked on the area where a click takes them
(`VisibleTimelineEdgeMarks`): green as long as there is media beyond, red at
the end of the media, as in Resolve (p. 1089). A click elsewhere in the tracks,
an undo, a change of timeline and `Échap` leave the mode; linked selection
makes the linked sound follow (`FollowLinks`). The **Trim** menu repeats these
commands.

(ON9) The centre of a cut between two adjacent clips (the cut and 2 points on
either side) does a roll without `Cmd`, with the Selection tool as with the
Trim tool; the cursor says so, as in Resolve: `]` on the end of the outgoing
clip, `[` on the start of the incoming clip, `][` on the cut that rolls. A
double-click on a cut takes it as a roll. In trim mode, when stopped, a press
on the Trim Monitor takes A (left image), B (right image) or a roll (the
divider, 24 points on either side of the middle); the drag moves the point by
six points per frame, as a preview, and release writes it in one undo step.

(ON8) A bar in the timeline's tool row shows the trim mode as long as the Trim
tool is the tool or a point is taken: with no point, **Prendre le raccord**
(Take the junction) (`V`); with a point, the state (ripple or simple, the
number of points), **A · Roll · B** (the side taken lit, a click chooses it),
**−5 −1 +1 +5**, **Étendre** (Extend), **Dynamique** (Dynamic) and **Quitter**
(Quit). The tooltip of each button gives its key; the buttons do what their
keys do, without taking the keyboard.

When stopped and in trim mode, the Record monitor becomes the Trim Monitor: the
last frame of A on the left, the first of B on the right, each whole within its
half of the frame (`LayoutTrimMonitor`), with above it its side and the frames
trimmed since the point selection (in green as soon as they move), below what
the trim can still take (`handle_frames`). Nothing but theme rectangles, atlas
text and the two textures. The two images often come from the same source
clip: the one missing from the cache is requested again until both are there.
Playback returns Record to the program.

The slide (name bar, Trim tool) goes through `SlideEditOperation`: the
neighbours absorb the move, and the linked sound follows when linked selection
is on, by the same rule as the MCP `slide_edit` (`SlideClipsWithLinkedPartners`,
`src/Operations.h`). The rate stretch goes through `RateStretchOperation`: the
clip keeps its source range, its speed becomes exact (range / duration) or the
gesture stops at the last number of frames that gives an exact speed and
overlaps nothing. With the Selection tool nothing ripples; with the Trim tool
(DEC40), the stretch ripples as in Resolve (p. 1219): the clip keeps its head,
whichever end is dragged, and what follows it shifts by the difference, on its
tracks and on the synchronized tracks (`RateStretchOperation::ripple`,
`TimelineTrimMode::RippleRateStretch`). Neither the slide nor the stretch snap.

The padlock in each header locks the track through a persisted, undoable
operation. A locked track is still rendered and readable under hatching, but
any mutation that targets it is atomically refused. The vertical divider of the
bin adjusts its width; the boundary between monitor and timeline slides
vertically to adapt the editing space.
