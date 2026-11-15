# Final video export

The **Fichier → Exporter la vidéo finale…** (File → Export final video…) menu
opens CUTMACHINE's render module, with stereo 48 kHz AAC audio. The dialog
offers five presets, the engine's single list (`Exporter::Presets()`), which
`--export --preset` and the `preset` argument of `export_timeline` and
`export_timelines` name by their identifier: **Haute qualité** (High quality,
`hevc_high_quality`), **Master x265** (`hevc_master`), **Web 1080p**
(`hevc_web_1080`), **Livraison UHD** (UHD delivery, `hevc_uhd_delivery`) and
**H.264 High — Diffusion** (H.264 High — Broadcast, `h264`). With no codec or
preset, every surface takes the engine's default: 8-bit H.264 High for an SDR
timeline, HEVC Main10 when colour management delivers HLG; the dialog opens on
this default. A preset that contradicts the named codec is refused. Drop-down
menus let you override the container, the resolution, the frame rate or the
encoding engine for a single export; no bitrate or timebase needs to be
entered by hand. **VideoToolbox** favours speed and the **software** encoder
(x264 or x265, depending on the preset's codec) constant quality.
The render runs off the interface thread, exposes its progress and can be
cancelled.

The equivalent headless command is:

```sh
./build/cutmachine --export Projet.cutmachine-project/project.cutmachine.json sortie.mp4
./build/cutmachine --export Projet.cutmachine-project/project.cutmachine.json sortie.mp4 --software
./build/cutmachine --export Projet.cutmachine-project/project.cutmachine.json sortie.mp4 --overwrite
./build/cutmachine --export Projet.cutmachine-project/project.cutmachine.json sortie-h264.mp4 \
  --codec h264 --software --entire --timeline '<timeline-id>'
./build/cutmachine --export Projet.cutmachine-project/project.cutmachine.json master.mp4 \
  --preset hevc_master
```

H.264 High is the default for an SDR timeline, on the CLI as on MCP;
`--codec hevc` selects HEVC Main10.

In a server (`--mcp-serve`, the app's relay), `export_timeline`,
`export_timelines` and `render_motion_video` take `async: true`: the response
carries a `task_id`, `get_task` reads the encoded frames out of the total —
for a batch, the file in progress and the number of finished files; an export
rendered in segments gives its pass ("segment 2/3", "assembly") and counts it
over the film's frames — then the tool's response, and `cancel_task` stops
FFmpeg and removes the partial file. The export works on the project as it was
at the call; `stale: true` says that the timeline changed before the end. A
relative output path is resolved against the folder that contains the project
package; `--call-tool` keeps the current folder.

Both export tools take `idempotency_key` (and `expected_revision`): sent again
under the same key, the call returns its first response, and with
`async: true` the same `task_id` (PRD40). A source clip that cannot be found is
refused before FFmpeg, as `MediaMissing`, with its path ("source media is
missing: <path>"), at export as for `read_frame`, the sheets and the colour
measurements.
The export keeps the definition and the rational frame rate of the targeted
timeline, over its entire duration. `--software` selects x264; VideoToolbox is
used by default. Durations that end between two export frames are refused.
`--tools` publishes the MCP catalogue and `--call-tool` invokes it without an
HTTP server, with the project lock and the same schemas and refusals. Each tool
there carries, as in `tools/list`, MCP `annotations` deduced from what it does
to the project (read, session, analysis, journaled edit, project edit, file
write, outside the journal, read and action off the machine): `readOnlyHint`
for a read, `destructiveHint: false` for what `undo` undoes,
`openWorldHint: false` except for After Effects. Only the
values that differ from the protocol's defaults are written. No tool
description exceeds 2,000 characters: Claude Code truncates beyond 2,048.

Each clip is placed at a whole frame number: FFmpeg evaluates no floating-point
division for a clip's position or activation window, which would shift about
one clip in ten by one frame. Each picture is read on its source's grid
(see [Time decisions](decisions-de-temps.md)): the input keeps the file's
timestamps (`-copyts`) after a seek one second ahead, and the clip's chain
subtracts the first picture's timestamp, resamples at the source's counted
rate (`fps=<rate>:start_time=0`) and trims to the first grid frame it shows,
so where the seek lands decides how much is decoded, never which picture is
shown. A clip at unit speed whose source is counted at another rate than the
sequence is handed the explicit list of frames the rule names, as a speed
ramp is. `verify_export` then compares the delivered file
with the document, frame by frame, and names the ranges that diverge: frame
offset (`offset`), a frame foreign to the clip (`wrong_picture`), a frame in a
gap (`picture_in_gap`) or a black frame (`black_frame`). The comparison works
on normalised luminance thumbnails, so a grade is not counted as a difference;
fades and translucent clips are declared unverifiable, and the frames of a
still clip that are too similar to their neighbours are counted as ambiguous
without ever being reported as offset.

`exact` is `true` only if the file has exactly the expected frames, each has
been compared and none diverges (PRD38). A single unverifiable frame — a fade,
a translucent clip or one whose opacity moves, a motion clip on one frame, the
ramp of a fade to colour, a source that is too short — leaves it at `false`,
and the report says next to it the share of frames compared
(`verified_fraction`, truncated to the thousandth, `1` only if all were) and
why (`not_exact_because`). `divergent_frames` at zero says that no compared
frame is wrong: an edit with fades answers `exact: false` without being wrong.
The ambiguous frames of a still clip count as verified: `exact: true` there
means "consistent with the document", and `ambiguous_frames` says how many
frames are not proven to the frame.

```sh
./build/cutmachine --call-tool Projet.cutmachine-project/project.cutmachine.json \
  verify_export '{"timeline_id":"<timeline-id>","path":"sortie.mp4"}'
```

`export_timeline` with `verify: true` makes the same comparison in the same
call and returns its report under `verification` (AGENT-2026-09 A7); the export
stands whatever the verification says. `timeline_stats` also counts the linked
picture/sound pairs and names, under `sync.out_of_sync`, those that do not read
their source clip at the same offset, with the exact drift (A5).

### Checking an edit against a policy

`check_timeline` (CLI: `--check-timeline <project> [--timeline <id>]
[--policy '<json>'] [--file <export>]`) judges a timeline without modifying
it, against a named policy (DONNEES-2026-10 OR4). It gathers in one call what
the LangGraph driver did in Python before it was removed: seven checks, each
`passed`, `failed` or `not_run` with its reason. A check that was not run is
never counted as passed.

| Check | Judged on | Fails when |
|---|---|---|
| `links` | the document | the picture and sound of the same source clip play together without being linked; each pair is named with its drift |
| `sync` | the document (`timeline_stats`) | a linked pair does not read its source clip at the same offset |
| `duration` | the document | the duration falls outside `duration_min` or `duration_max`; not run without a bound |
| `final_black` | the document, and the file if given | a frame at the end is black or on its way to black |
| `caption_contrast` | the file only | a caption is under the minimum contrast, measured like `check_caption_legibility`; not run without a file |
| `charter` | the document | a font or a colour falls outside the brand kit that `kit` names; not run without `kit` (MOTION-BIBLIOTHEQUE-2026-10, DEC6) |
| `jump_cuts` | the document | a cut inside a single source clip shows as it is; not run unless `jump_cuts` is `forbidden` |

Two more checks appear in the report only if the policy asks for them (PA2 and
PA11, PARITE-2026-11), so that a report without
them keeps its seven checks:

| Check | Judged on | Fails when |
|---|---|---|
| `loudness` | the file only | the file's integrated loudness differs by more than one unit (1 LU) from `loudness_target`, its true peak exceeds `true_peak_max` (−1 dBTP by default), or the file is silent; not run without a file |
| `format` | the document | the sequence does not have the shape of `aspect_ratio` (`"9:16"`) |

The default policy is called `default`: final black forbidden, captions at
3:1, jump cuts allowed, no duration bound. All its keys are optional: `name`,
`duration_min`, `duration_max`, `final_black` (`forbidden` or `allowed`),
`jump_cuts` (`forbidden` or `allowed`), `final_window` (in whole frames, one
second by default, rounded up to the frame: 30 frames at 29.97),
`minimum_contrast` and `kit`, the name of a brand kit from the library
(`list_motion_templates`, `kits`), `loudness_target` (whole LUFS, from −40 to
−5), `true_peak_max` (whole dBTP, from −20 to 0, with `loudness_target` only)
and `aspect_ratio`. A policy changed without a name is called `custom`, and the
report repeats it at the top.

**Jump cuts** (`jump_cuts`, outside the batch, from the benchmark against
another editor). A cut inside a single source clip is a jump cut when the film
shows it as it is. The same source clip is needed on both sides, with no
continuous join, with the same fixed frame. It is also needed that, on the
frame before the cut and on the cut frame, the composed image be that track's
clip and nothing else. A cutaway that covers the cut, or that starts or ends on
it, hides it. A dissolve, a `dip_to_color` or a partial-opacity clip mix two
images and hide it too. A reframing on one side breaks it, animated frame
included: this is the zoom an editor adds in Resolve to break a jump without an
illustration. A motion clip is drawn over the cut, not in its place: a title or
a logo does not hide a jump. The cuts are those of `review_cuts`, which judges
the jump on the pixels; this check counts them all on the document. The report
always lists them under `jump_cuts` (frame, position, track, the two clips, the
source clip), whatever the policy. The check judges them only if the policy
forbids them: a jump cut can be a choice (principle 12), and it is the rule for
an interview, not for a reel.

**The brand kit** (`charter`). The check reads, in the document whose file is
exported, the font and colours (text, outline if it has a width, shadow if it
has an offset, background) of each caption style that a visible caption uses —
a caption without a style is judged on the values of a new style, as the export
draws it —, and the fonts and colours of each motion clip on a visible track:
styles, text ranges, repeat cycles, animation keys, gradient stops, the colour
of a filter or of a text outline, values of the instance's props, and what the
engine takes when nobody wrote anything (a text without a colour is black,
without a family `system-ui`; a path with no fill is filled with black). A node
that the instance hides does not count. These are the written values, never
the blends an animation passes through: a fade from the kit's blue to its white
passes through blues that it does not list, and nobody chose them. A colour is
compared by its 8-bit RGB, opacity apart; a font by its name, case-insensitive.
The kit allows the colours and fonts of its props, of its palette, of its
`fonts` and of its captions (`MotionKitColors`, `MotionKitFonts`). Each value
outside the kit is named where it is written (`outside`: the `style_id`, the
setting, the number of captions and the first; or the `clip_id`, the
composition, the node and the property), and `outside_values` gives it once
with the number of places. The file's pixels are not read: an HLG delivery or a
grade moves them, and `verify_export` proves that the file is the timeline. An
instance of another composition is judged where it is placed (`not_read`). The
report gives the kit and its version; a kit that the library does not have is
refused (`UnknownKit`). The document does not keep the kit, only the values
given (a project reopened without the library renders the same film, and only
this check no longer runs).

Final black is read frame by frame over the end of the film, composed as the
export composes it: from the highest visible track downwards, the first one
that shows a layer wins. A frame with nothing is the black of the background,
a full-strength black `dip_to_color` is black, a black veil on its way or a
clip whose opacity drops above nothing is a fade; each range names its cause
(`nothing_on_screen`, `dip_to_color` with the transition, `clip_opacity` with
the clip). A veil of another colour is not black. A motion clip counts as an
image: the document does not see inside its composition, and `motion_frames`
says how many frames of the window can be judged only on the file. With `path`
(`--file`), the last frames of the file are read by FFmpeg, reduced to grey,
and judged at `blackdetect`'s default thresholds (98 % of pixels under a tenth
of the scale): this is how the benchmark measured "no fade to final black". On
a teaser, it shows two black frames that its document does not see, inside its
composition.

Beside the checks, and judged by none, the `generated` list names each clip of
the timeline whose media was made by a generation service (GENERATIF-2026-10,
DEC36): `clip_id`, `track_id`, `media_id`, `timeline_in`, `duration`, and what
its provenance says (`service`, `model`, `kind`, `created_at`). It is always
there, empty when nothing is generated: the agent's report draws its line per
generated clip from it. A generated clip is not a fault, it makes no check
fail.

The CLI sets `"ok"` to the verdict's value, like `--check-setup`, and exits
with code 1 when a check fails. A relative path given to the server is read in
the project folder, as for `verify_export`.

```sh
./build/cutmachine --check-timeline Projet.cutmachine-project/project.cutmachine.json \
  --policy '{"duration_max":{"value":60,"rate":1}}' --file sortie.mp4
./build/cutmachine --check-timeline Projet.cutmachine-project/project.cutmachine.json \
  --policy '{"kit":"marque"}' --file sortie.mp4
```

For a musical edit, `analyze_audio_beats` produces the onset cache of an audio
source clip, then `list_audio_beats` reads them by exact source range. The
result bounds the number of onsets and provides `next_source_in` to continue
reading. The tempo and strengths are estimates; the existing cutting tools then
place the chosen frames.

```sh
./build/cutmachine --call-tool Projet.cutmachine-project/project.cutmachine.json \
  analyze_audio_beats '{"media_id":"<ulid-rush-audio>"}'
./build/cutmachine --call-tool Projet.cutmachine-project/project.cutmachine.json \
  list_audio_beats '{"media_id":"<ulid-rush-audio>","max_onsets":64}'
```

`list_music_structure` reads, on the grid of `list_tempo_grid` (same
arguments), the energy of each beat in three bands: bass under 150 Hz, treble
above 4 kHz, and the whole spectrum. It names the beats where the bass rises
(`rise`, the bass comes in: a drop) or falls (`fall`, a break) by at least 6
dB. On a reel's track at 124 BPM, the bass comes in at 15.2 s (+20.3 dB),
leaves at 79.1 s and comes back at 84.9 s. The bands are measured during
`analyze_audio_beats`, on the same spectra as the onsets, with no second
decode; an older cache requires running this analysis again. Measured, never
judged: whether to cut on a drop is up to the author.

`list_clip_tempo_beats` (same grid arguments as `list_tempo_grid`) places a
source clip's grid on the timeline: for each clip that reads this media, the
sequence frame of each beat, and the exact gap between the beat and the start
of that frame, in samples and in milliseconds. Rounding to the frame is done
in a single place, so the tool answers with the frame the engine would take. At
the junction of two adjacent clips of the same media, it says whether the grid
crosses the cut; otherwise, it names the offset in samples, and `beat_offset`
the number of beats the cut skipped. It reads without modifying anything: the
caller places the cuts.

The `clone_timeline` tool remaps the objects' IDs and reuses the sources; it is
undone with `--undo-project-op` and redone with `--redo-project-op`.
`create_timeline_variants` generates dates × formats in one project operation:
the occurrences of the text are counted and the whole batch is refused if a
template no longer matches. `export_timelines` validates all the timelines and
their destinations before the first render. These two tools offer a preview by
default; `"preview":false` runs the batch. The identifiers and output paths are
explicit.

The export composes the video tracks in their order, keeps the black areas,
respects the sources' rotations and ratio, then mixes the audio clips without
automatic normalisation. The visible caption tracks are composed with their
literal UTF-8 text, font, size, colour, position and opacity, over their exact
ranges. Line breaks are explicit and a caption's lines are centred; MOGRT
templates remain absent from this render. Without colour management in the
project, the monitor's `color.*` effects (eight settings, the lift/gamma/gain/
offset wheels, the curves, the hue rotation and the hue curves) are applied in
each video clip's order before composition, through temporary LUTs shared
between identical grades. The clips there are composed in 10-bit 4:4:4, as
under colour management (DEC28): an edge on an odd row or column falls where
Record draws it, and a 10-bit delivery keeps its 10 bits up to the output
format, reached only once at the end of the chain. An untagged BT.601 source
comes out tagged BT.709, its values passing as they are: this is the rule. It
first writes a neighbouring temporary file and replaces the destination only
after a successful encode.

Under colour management, each clip receives before composition a single 65³ 3D
LUT built with OpenColorIO from the built-in ACES Studio configuration: input
signal → working space, grade where the monitor applies it (ACEScct), then back
to linear ACEScg. The clips are composed in floating point in this linear
space, and the display transform applies only once, after the last layer: a
fade or a translucent clip blends in light, as on the monitor. A graded clip is
therefore no longer refused under colour management, and it is no longer
necessary to turn colour management off to export a grade. For a Sony source
clip, the chain is S-Log3/S-Gamut3.Cine → ACEScct → ACEScg → Rec.2020 HLG. The
HEVC Main10 stream is explicitly tagged BT.2020 non-constant, ARIB STD-B67 and
limited range. An image that its file does not tag (an FX30 writes no matrix in
its MP4) is read as on the monitor: BT.709 matrix, and full range under an
S-Log3 input. swscale, left to itself, read it as BT.601: 0.011 to 0.019 less
green on a face than in Record.
The sheets (`contact_sheet`, `cut_sheet`) and `read_frame` go through the same
colour view as the file, but show a clip's source: neither its grade, nor the
composition of the tracks (except with `graded: true`, which shows the grade
and the look, still without the composition). To judge the delivered image,
read the export back (`verify_export`).

A grade set without colour management does not transpose as is: on a real edit,
settings made to compensate for the absence of conversion overshoot once it is
enabled. These are the settings to redo.

## Captions

`generate_captions` (MCP) builds the captions from the edit's words (aligned
boundaries when they exist, unheard words discarded) and places them on a new
sync-locked caption track, in a single reversible operation. Each caption fits
on one line: a syntactic unit of around 18 characters per second, cut at
sentence ends and long pauses, of `max_characters` at most and never wider than
`max_line_width` **as the export draws it**, outline included. By default, the
number of characters follows the timeline's format: 32 in landscape (wider than
tall), 20 otherwise, in vertical and, for lack of a delivered film to decide,
in 1:1 and 4:5. These are the settings of delivered films: a 32-character line
in 16:9, 20 per caption in vertical (DEC4,
`REGLES-2026-10`, extended to landscape). A
given `max_characters` keeps its value. The width is by default 60 % of the
frame's; the SRT of `--export-srt` keeps 42 characters. The width is measured
by the engine that draws the captions (see below), not estimated by a number of
characters. A caption edge within `snap_frames` frames (12 by default) of a
visible cut snaps to it, unless the caption would lose its legibility there or
the edge would fall inside one of its words: a caption opens at the latest at
the start of its first word and closes at the earliest at the end of its last
(DEC18, `BENCH-2026-09`).
`caption_gaps` lists the gaps between two captions during which a word is
spoken, with its reason: `unheard` (a word that no reading of the edit hears)
or `aligned_in_removed_range` (a word heard at the join, which the aligner
placed in the cut material).
Each caption is returned with its characters, its approximate rate, its width
and its distance to the nearest cut; `preview:true` measures without editing.
The text does not follow a cut by itself: after removing words, run the
generation again.

**A track in another language** (PA5,
`PARITE-2026-11`). `translate_captions`
places a translation next to a caption track: the agent gives one text per
caption (`cartons`, `{clip_id, text}` pairs), the engine places them on a new
track at the same times and in the same styles, in one operation.
Each caption comes back with its characters, its rate
(`characters_per_second`), its width as the export draws it and `wraps` when it
exceeds the delivery's line (`max_line_width`, nine tenths of the frame): the
engine measures, it shortens nothing. A caption without text is refused by
name. `preview:true` measures without editing.

The new track is **hidden** by default. The export draws all the visible
caption tracks: two languages left visible would come out on top of each other.
A version is delivered by showing its track and hiding the others
(`set_track_output`). The language is not in the document, which has no field
for it: it is kept in the journal with the edit (`list_motifs`, rule
`translate_captions`). In a process without caption rendering (a test, the
windowless build), the widths are not measured and the response says so
(`widths_measured: false`).

**Where a caption stops** (RM3,
`REGLES-2026-10`). The hand-written rule ranks
the possible boundaries: cost according to the words on either side,
punctuation and pause. A learned model can correct it, within the same bounds:
characters, duration, sentence ends, long pauses. `train_caption_breaks` trains
it on delivered subtitle tracks (one file per corpus, one track per line, the
word times estimated or given) and measures it against the rule on groups
excluded from its training and on an evaluation corpus that it never trains on
(`evaluation`). With `install:true`, which requires this evaluation, it
installs it if it reproduces more delivered captions, with a better F1 of the
boundaries, on both sides, and if it respects the two firm choices (the article
stays with its noun, the conjunction opens the next caption). The forge
measures only the splitting: before installing, replay `generate_captions` on
the films of the criterion. On the corpus used so far, no model beats the rule,
and none is installed. The installed model lives in the engine's data folder
(`models/caption_breaks.json`), never in the project or in the repository;
`CUTMACHINE_MODELS` moves it, `off` turns it off. As soon as it is installed,
`generate_captions` cuts with it. Its `break_model` response says what cut:
`source` is `rule` or `model`, with the weights' fingerprint in the second
case. `break_model:"rule"` asks for the rule alone. An installed file that is
unreadable is reported (`model_error`), and the rule decides. The document
keeps only the captions: reopening the project elsewhere, without the model,
gives the same edit.

**Captions split by the caller.** `cartons` gives the grouping in place of the
split: texts whose words are, in order, those of the edit (the spans of
`get_timeline_transcript`, up to case and typographic apostrophes). The engine
places the times as for its own split, the capitals, the width, the snapping to
cuts, and never merges a short caption into its neighbour. It refuses, naming
the caption and the word's index, a word that differs or is missing, a sentence
end or a 0.7 s pause inside a caption, and a multi-word caption beyond
`max_characters`. `break_model.source` is then `cartons`. An agent that splits
from the words, without seeing the engine's proposal, was preferred in a blind
comparison (REGLES-2026-10, RM3).

A caption style (`add_caption_style`, which returns its `style_id`) accepts an
outline (`outline_width`, `outline_color`), a shadow (`shadow_offset`,
`shadow_color`), a background (`background_color`, `background_padding`) and an
exact vertical position (`vertical_position`, a fraction of the height measured
from the top, which places the centre of the text block above the applications'
interface). These fields are written in the project only when they are set: an
existing style keeps its bytes. `update_caption_style` modifies a style in one
reversible operation, and all the captions that use it follow. The bounds are
those of `Document.h` (`kCaptionStyleMaxOutlineWidth` and
`kCaptionStyleMaxShadowOffset`, 256 px, `kCaptionStyleMaxBackgroundPadding`,
512 px), which the document's validation applies (`ValidateCaptionStyle`).

A style also names its weight (`font_weight`, from 1 to 1000 as in CSS; 400 by
default, regular, 500 medium). The face is chosen as for a motion text
(MOTION-WEB-2026-10 MW7): the face of the family that CSS would take, or the
`wght` axis of a variable font. Without it, a caption would always be drawn in
regular, whereas a house style set in Resolve can be medium. The field is
written only if it is not 400: an existing project keeps its bytes, and an
older binary reads the project back ignoring the key, hence in regular. A brand
kit can set it (`captions.font_weight`). The Inspector does not show it yet.

A style can animate a caption's words (`word_animation`, PARITE-2026-11 PA4):

- `highlight` draws the word being spoken in another colour
  (`highlight_color`, `#ffe600` by default). The word stays lit until the next
  one, and the last until the end of the caption.
- `reveal` draws each word from the moment it is spoken. The line is placed as
  if it were full: it does not move as it fills. A background
  (`background_color`) is drawn whole as soon as the caption opens.
- `none` (the default) draws the whole caption.

Only a caption whose words are timestamped is animated. `generate_captions`
writes each word's time on each caption (`caption_word_times` in the file,
`timed_words` in `describe` and in its response). A caption imported from an
SRT, typed by hand or translated (`translate_captions`) has none: it is drawn
whole, whatever the style.

The times are counted on the caption's clock, which starts at its start. They
follow the edit: a caption moved, trimmed, rolled against its neighbour or cut
in two says each word at the instant it was spoken, and an undo returns the
same bytes. `set_clip_caption` keeps the times when the new text has as many
words (a typo fixed, a style changed) and removes them otherwise; you must then
run `generate_captions` again for the caption to animate again.

At export, an animated caption is one frame per word instead of one: the word
changes on the frame where it starts, on the monitor as in the file. What is
not done:

- nothing has been seen on screen or on a film; the tests read the pixels of
  the render and of an exported file;
- the Inspector does not show the setting;
- `check_caption_legibility` measures the text colour, not that of the lit
  word;
- an SRT, a VTT and the hand-off to Resolve do not carry the animation;
- there is no continuous animation (a word that grows or slides): a word is lit
  or it is not;
- right-to-left writing is not handled.

`add_caption_style {kit}` starts from the named brand kit (`brand-kit.v1` with
`captions`) (DEC6): its settings, then those of its caption format of the
timeline's exact size. The settings given alongside take precedence and are
returned under `kit.overridden`; `kit.set` says what the kit set and
`kit.format` the chosen format. Refusals: `UnknownKit`, `KitMismatch` for a kit
without captions, `UnknownFormat` when its caption formats have no layout of
the timeline's size (its sizes are pixels of those frames). The style keeps the
values, not the kit.

In the window, the "Style de légende" (Caption style) section of the Inspector
(F2.3) sets the whole style of the selected caption, under its text. It holds
the font, the size, the colour, the outline, the drop shadow, the background and
the position (three presets or a chosen height, as an exact percentage of the
frame). Each gesture makes an `UpdateCaptionStyleOperation`, and a drag of the
height slider makes only one, on release. This is where Resolve sets a subtitle
track's Track Style. The **Légendes** (Captions) tab of the **Média** panel
keeps the list of styles, their creation, their deletion and the dressing of
the captions. None of this has yet been looked at on screen.

`check_caption_legibility` measures legibility in the exported file. On the
middle frame of each caption, it finds the letters with the export's drawing
and compares the text colour with the ring of pixels around them (the outline
if there is one, the image otherwise): median WCAG contrast ratio and tenth
percentile for the worst passage, 3:1 minimum by default (`minimum_contrast`).
White text on a white shirt measures about 2:1; an outline brings it above
10:1. On a real edit, it found by itself 11 captions out of 58 under 3:1. A
relative `path` given to the server is read in the project folder, as for
`verify_export` and `check_timeline` (DEC19,
`PRODUIT-2026-10b`); `--call-tool` keeps the
current folder.

A caption is drawn once, by the engine, with Core Text
(LEGENDES-2026-10), and not by FFmpeg's
`drawtext` filter. The export lays this drawing on the image, the Record monitor
shows the same one at its size, and the measurement of lines like
`check_caption_legibility` read the same layout. The meaning of a style is
unchanged:

- the outline is outside the letters;
- the shadow is offset down and to the right and carries the outline;
- the background surrounds the text with its margin;
- `position` keeps one twentieth of the height as margin;
- `vertical_position` places the centre of the block;
- a caption that would leave the image is brought back into it;
- the colours are written as they are in the delivered signal.

The export therefore no longer needs its FFmpeg to have `drawtext`. Outside the
captions, the image has the same bytes with or without captions.

A family is looked up as for motion: installed fonts, registered folders, and
those that Adobe Fonts activates as long as Creative Cloud is running. A
caption takes the face its style's `font_weight` names (see above), and in a
variable font the instance at that point of its `wght` axis (MOTION-WEB-2026-10
MW7). When the family cannot be found, the caption is drawn in Helvetica Neue,
on the monitor as in the file. The export does not refuse it: its response
carries `caption_fonts_substituted`, each family with the one that replaces it
(`[{"family":"Poppins","exported_as":"Helvetica Neue"}]`, absent when nothing
is missing), on MCP, on the CLI and in the preview of `export_timelines`. The
window warns before starting the export ("Exporter quand même" (Export anyway)
or "Annuler" (Cancel)). `check_caption_legibility` (`fonts.missing`) and
`list_fonts` (`captions.missing`) say it beforehand.

Record and the file have the same drawing, but **the monitor has not yet been
rechecked on screen**.

## Delivering at a loudness

> PA2, PARITE-2026-11. Measured on synthetic
> sounds; no film delivered this way, nothing listened to.

By default, the export mixes the sounds without normalisation: the film comes
out at the level that its source clips and its music add up to.
`export_timeline` and `export_timelines` take `loudness_target`, in whole LUFS
from −40 to −5 (−14 for the web, −23 for broadcast); on the command line,
`--export … --loudness -14`.

The target belongs to the delivery, like the codec: it is not in the document,
and the monitor, `transcribe_timeline` and `analyze_timeline_audio` keep
hearing the mix as it is edited. The export proceeds in three steps:

1. it renders the film as usual, the sound kept lossless (24-bit ALAC);
2. it measures this sound like a broadcast loudness meter (EBU R 128,
   FFmpeg's `ebur128` filter, the one of `analyze_timeline_audio`);
3. it applies a single gain toward the target, runs the mix's limiter again
   (peak at −3.5 dBFS), encodes the sound only once to AAC and copies the image
   as it is.

The measurement is made on the rendered file, not on a prediction: the sound of
a motion clip and a film rendered in segments are in it. The response carries
`loudness`: the target, the mix's loudness and peak before the gain (`mix_lufs`,
`mix_true_peak_dbfs`), the gain (`gain_db`), then the loudness and peak of the
delivered file, measured again (`delivered_lufs`, `delivered_true_peak_dbfs`).
An upward gain can make the limiter work: the file then comes out a little
under the target, and `delivered_lufs` says so. A silent film is delivered as it
is (`silent: true`).

`check_timeline` then judges the file: see `loudness_target` in the policy,
above.

## Measuring the edited sound

`analyze_timeline_audio` (MCP) measures a timeline's sound as it plays,
assembled as `transcribe_timeline` and the export do, without exporting
anything: integrated loudness, true peak and loudness range (EBU R128), level
and peak of the first seconds with the air before the first voice, and each dip.
A dip is a portion closer to the noise floor of the source clip that plays it
than to its speech level (`dip_position_percent`, 50 % by default), or a hole
where no sound plays. The floor and the speech level come from
`analyze_speech_onset`; a source clip without this analysis is listed in
`sources_without_floor` and its passages are not judged. No absolute threshold
in dBFS is used: on the measured edit, the floors ranged from −36 to −45 dBFS
under speech at −11, and a room at −25 dBFS hides any dip under −30.

```sh
./build/cutmachine --call-tool Projet.cutmachine-project/project.cutmachine.json \
  generate_captions '{"timeline_id":"<timeline-id>","preview":true}'
./build/cutmachine --call-tool Projet.cutmachine-project/project.cutmachine.json \
  check_caption_legibility '{"timeline_id":"<timeline-id>","path":"sortie.mp4"}'
./build/cutmachine --call-tool Projet.cutmachine-project/project.cutmachine.json \
  analyze_timeline_audio '{"timeline_id":"<timeline-id>"}'
```
