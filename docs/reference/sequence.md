# Sequence

The timeline has a `sequence` object independent of the source media, with a
ULID, a name, a width, a height and a rational frame rate. This object
directly owns its `tracks` and its `markers`: no timeline floats at project
level without a sequence. As in Premiere Pro or Resolve, this canvas drives
the monitor and serves as the basis for the export presets; a portrait video
therefore does not implicitly transform a horizontal sequence.
**Timeline → Réglages de séquence…** (Timeline → Sequence settings…) offers
the HD, UHD, vertical and square formats as well as the usual frame rates.
Older documents with no `sequence` block, or earlier than version 4, are
refused.

The application loads a complete `Project` around this active document. All
its timelines appear in the bin; a double-click activates one and
`Cmd+Option+N` creates a new one. An autosave of the complete project
protects each atomic commit and can be recovered at launch.

`--describe` writes only the condensed JSON view to stdout, with the distinct
`timeline` and `library` blocks. Library media have aliases `M1`, `M2`, etc.
and remain available when they are edited in (`in_use:true`). `--ingest`
reads the FFmpeg headers without decoding the picture (only the sound is
decoded, to measure its average level), keeps the exact rational frame
rates, keeps the display matrix rotation and deduces the displayed
orientation from it. The player also re-probes the edited-in sources at
launch: the information banner exposes codec, dimensions, orientation,
rotation, frame rate and audio presence. Portrait videos are rotated, then
fitted to the monitor with their ratio preserved and neutral bars, without
stretching. This presentation cache does not mutate the document from the
interface. Files that are not media, or are corrupted, are reported in
`errors` without making the batch fail; the idempotent identity is the
resolved absolute path.

The current document schema runs from version 7 to version 26 (each version
adds what it introduces: the `kDocumentVersionWith…` constants of
`src/engine/Document.h`): a document is
written at the oldest version that carries everything it contains, and
versions 4 to 6 are still read. `library`, `bins` and `sources` are project
objects; `tracks` and `markers` belong to `sequence`. Versions 1 to 3 are
refused. Library and source share the media's ULID; ingest alone never
places a clip on the timeline.

## Colour management

The **Couleur** (Colour) menu offers a direct preset **Sony S-Log3 →
Rec.2020 HLG** and an advanced panel. The chain is persisted in
`color_management` at project level: input gamut and curve, YCbCr matrix,
Full/Legal range, grading space, then output gamut and curve. The preset
uses the Full range required by the Sony specification, S-Gamut3.Cine/S-Log3,
ACEScct/AP1 as the wide-gamut grading space and a Rec.2020/HLG output. The
range and the matrix can also automatically follow the FFmpeg metadata of
each frame.

Each planar YUV frame of 8 to 16 bits is first normalised according to its
depth and its range. OpenColorIO 2.5.2 then applies the transforms of the
built-in, frozen configuration `studio-config-v2.2.0_aces-v1.3_ocio-v2.4`: no
`$OCIO` variable and no external file takes part in the rendering. The player
samples the OCIO processors into two 65³ 3D LUTs cached by Metal, before and
after the ACEScct grading; track compositing stays in linear AP1 in a 16-bit
float texture. Export uses the same OCIO processor in the form of a `.cube`
LUT. The editing monitors derive a local Rec.709/SDR view from the delivery
setting so that macOS does not present the HLG master as an EDR layer in the
middle of the interface. This display preference changes neither the document
nor the exported render.

An SDR delivery goes through the "ACES 1.0 - SDR Video" view of the
configuration. An HLG delivery does not go through ACES's HDR view, which put
18% grey at 30% of the signal (about 15 nits). It follows the curve that
DaVinci Resolve renders with the reference settings (BLANC-HLG-2026-10 BH3,
`HlgDelivery.h`):
- a toe in the blacks;
- 18% grey at 37% of the signal, as in BT.2408;
- a shoulder that reaches the peak at about 17 times the white.

This curve is applied channel by channel in DaVinci Wide Gamut, then encoded
in HLG with the reference white at 75% of the signal.

To judge a grade, the **Couleur** menu also offers to ignore the grade at the
Record monitor (Shift+D: clips without their `color.*` stages, sequence
without its look) and to compare it with a reference still taken on Record
(Cmd+Alt+G), shown on the left half of the frame (Cmd+Alt+W). These are local
monitor preferences: neither the document, nor the export, nor the
measurements change.

Settings go through the reversible `SetColorManagement` operation, also
available via `--apply-op` and the MCP tool `set_color_management`. An input
gamut/curve combination absent from the built-in configuration is refused
before rendering rather than silently replaced.

The `hlg` input accepts source clips already shot in Rec.2020 HLG (ARIB
STD-B67 curve), like those of a camera set to HLG rather than S-Log3. It is
only valid with the `rec2020` gamut.

Each media item can declare its own input, which replaces the project's for
that item alone: a phone video in Rec.709 or in HLG in an S-Log3 project is
read in its own space, at the monitor, at export, in `read_frame`, the
contact sheets and `measure_color`. Ingest declares it when the file says so:
the metadata of a Sony camera (S-Log3 / S-Gamut3.Cine, `rtmd` track),
otherwise the container tags (HLG in BT.2020, or BT.709), never tags that a
house curve announced by the container contradicts (a Blackmagic Pocket 4K
tags its Film log as BT.709). A media item that declares nothing follows the
project. The reversible `SetMediaColorInput` operation, the MCP tool
`set_media_color_input` (`from_file:true` rereads the ingested files first)
and the **Espace couleur d’entrée** (Input colour space) context menu of the
media browser set it; `describe` lists it as `color_input`. A document that
carries one goes to version 16.

A media item can also be declared **graphic** (DEC31): a logo, a packshot, a
still image. Its code values are then what it shows, like the colours of a
motion title: under colour management, it is placed in the graphics view and
blended there (`OpenColorIoCompositeDisplayReferred`), instead of being read
through a camera's input; a white comes out white in SDR and at the peak of an
HLG delivery. Without colour management, only the matrix of an RGB graphic
(a PNG) changes, converted to BT.709. `set_media_color_input` with
`graphics: true` declares it (`input_ycbcr_matrix` and `input_range` say, if
needed, how to read a YCbCr graphic), as does the menu **Espace couleur
d’entrée › Graphisme (logo, image)** (Input colour space › Graphic (logo,
image)); ingest never guesses it. Under colour management, a graphic takes no
grade: export refuses a graphic clip that carries a `color.*` stage,
`measure_color`, `match_color` and a graded `read_frame` refuse it, naming it,
`propose_exposure` and `propose_grade` leave it aside, and the graded contact
sheet shows it as it is written, without the sequence look.
`conform_color_management` does not count it. A document that carries one
goes to version 21.

`conform_color_management` (MCP) sets the project input to what the media
declare, each by its own declaration, otherwise by its tags: HLG or S-Log3
turns management on with that input, Rec.709 sets the `rec709` input without
touching the activation. Media that declare nothing decide nothing: the tool
says so and changes nothing, and `set_color_management` is what has to be
used. The working and output settings are kept; mixed tags are listed in
`declarations` and the most frequent input wins. `preview:true` returns the
proposal without applying it.

`--apply-op` reuses the canonical format of `SerializeOperation`, replaces
the document transactionally and keeps the journal in the companion file
`<document>.timeline-<timeline-id>.editlog.json`, one per timeline. In case
of refusal, neither the document nor this journal is modified.

`--apply-project-op` applies in the same way the multi-timeline, bin metadata
and relink operations. Its separate history is kept in
`<document>.project-editlog.json` and the `--undo-project-op` and
`--redo-project-op` commands walk through it without initialising the
graphical interface.

The objects passed to `--apply-project-op` must reproduce exactly the order
and all the fields produced by `SerializeProjectOperation`.
`exact_project_hex` is `null` for a new intention; the journal replaces it
with `exact_project_delta`, the exact delta of what the operation changed,
for an exact replay. Older journals still carry a complete project under
`exact_project_hex` and are still read. Such a journal (DEC24) is rewritten in
delta form as
soon as the editor or `open_project` opens the project, under the session
lock, by the package transaction; the rest of the package is not touched.
Only a change that no delta expresses (a reordering) keeps a complete
project. Complete canonical examples (the identifiers are ULIDs):

```json
{"type":"AddProjectTimeline","name":"Vertical","width":1080,"height":1920,"frame_rate":{"num":25,"den":1},"timeline_id":"","video_track_id":"","audio_track_id":"","exact_project_hex":null}
{"type":"CreateProjectTimelineFromSegments","name":"Extrait","width":1920,"height":1080,"frame_rate":{"num":25,"den":1},"segments":[{"source_id":"01K00000000000000000000001","source_in":{"value":0,"rate":25},"duration":{"value":50,"rate":25},"video_clip_id":"","audio_clip_id":"","link_group_id":""}],"timeline_id":"","video_track_id":"","audio_track_id":"","make_active":true,"exact_project_hex":null}
{"type":"RemoveProjectTimeline","timeline_id":"01K00000000000000000000002","exact_project_hex":null}
{"type":"SetProjectBinMetadata","item_id":"01K00000000000000000000001","metadata":{"description":"Interview retenue","rating":5,"tags":["interview"],"insert_order":0,"display_name":"Interview principale"},"exact_project_hex":null}
{"type":"SetProjectTimelineBin","timeline_id":"01K00000000000000000000002","bin_id":"01K00000000000000000000003","exact_project_hex":null}
{"type":"RenameProjectItem","item_id":"01K00000000000000000000002","name":"Montage vertical","exact_project_hex":null}
{"type":"SetActiveProjectTimeline","timeline_id":"01K00000000000000000000002","exact_project_hex":null}
{"type":"ChooseProjectTimeline","timeline_id":"01K00000000000000000000002","alternative_timeline_ids":["01K00000000000000000000004"],"make_active":false,"exact_project_hex":null}
{"type":"RelinkProjectMedia","replacements":[{"media_id":"01K00000000000000000000001","replacement":{"id":"01K00000000000000000000001","path":"rushes/interview.mov","filename":"interview.mov","codec":"h264","width":1920,"height":1080,"pixel_format":"yuv420p","color_range":"tv","color_space":"bt709","color_transfer":"bt709","color_primaries":"bt709","rotation_degrees":0,"rate":{"num":25,"den":1},"duration":{"value":250,"rate":25},"orientation":"landscape","has_video":1,"has_audio":1,"audio_rate":48000,"audio_channels":2,"bin_id":"","proxy_path":"","metadata_complete":1},"stored_path":"rushes/interview.mov"}],"exact_project_hex":null}
```
