# Import from DaVinci Resolve

A Resolve bin is not a file: it lives in the project's database, and a `.drp`
is an opaque archive. There is therefore nothing to parse — the only honest
input is to make Resolve do the talking.

## From Resolve, in one click

```sh
tools/resolve-plugin/install.sh
```

Then in Resolve: `Workspace → Scripts → Utility → CUTMACHINE`. The window
imports the bins of the open project and can query the engine (`--describe`)
to show what the agent will see. See
[`tools/resolve-plugin/README.md`](../../tools/resolve-plugin/README.md).

This is the recommended path: the script runs **inside** Resolve, so it does
not need the external scripting reserved to Resolve Studio, and needs no
Python interpreter.

## From a terminal

```sh
# 1. Resolve Studio is running, the project open. The bridge reads the Media Pool.
python3 -m sidecar.resolve_bridge -o manifest.json

# 2. CUTMACHINE reproduces the tree and ingests the source clips.
./build/cutmachine --import-resolve \
  ./Film.cutmachine-project/project.cutmachine.json ./manifest.json
```

This route requires **DaVinci Resolve Studio**: the free version only runs
scripts from inside the application. In return, it can be automated.

Both producers write the same manifest schema and go through the same
importer — verified on a project of 408 source clips, where the two manifests
are equivalent. Neither modifies the Resolve project: they read the Media
Pool.

The manifest carries **only identity and organisation** — path, name, bin. No
technical metadata: Resolve returns its frame rate as a float, and a float
never becomes a `RationalTime`. The FFmpeg probe of `--ingest` is the
authority on frame rate and duration, so a source clip imported from Resolve
is identical to the same source clip ingested from a folder, loudness
included: the import measures it as `--ingest` does (DEC32), and a silent clip
is recognised before Whisper.

Bins are created by `AddBinOperation`, new source clips each enter their bin
through a single `IngestMediaOperation`, and source clips already known are
reclassified by `SetMediaBinOperation`: the whole import is undone with
`Cmd+Z` or the `undo` tool, step by step. Re-importing the same manifest
duplicates nothing — bins are matched by name and parent, source clips by
resolved absolute path. A Media Pool that grows can therefore be re-imported,
adding only what is new. Note: a source clip already present is reclassified
into its Resolve bin, so a re-import overwrites the classification made by
hand in CUTMACHINE.

Entries without a file — timelines, compound clips, generators — are set aside
by the bridge and reported in its `skipped`. On the import side, every source
clip refused is **named with its reason** in the `errors` array, as for
`--ingest`: on a Media Pool of several hundred source clips, a bare counter
would force a manual comparison with the bin. A source clip already in the
media library counts in `skipped` without appearing in `errors` — it is the
expected result of a re-import, not a failure.

```json
{"ok":true,"added":406,"skipped":2,"bins_created":22,"bins_reused":0,
 "detail":"<nom du projet Resolve>","errors":[]}
```

Offline media is still refused: the volume must be mounted at the time of the
import. Audio-only files — voice-over, music, ambience, stock sound — are
ingested normally with `has_video: false`. Their exact frame rate is that of
the audio samples; the image commands (`read_frame`, quality analysis,
thumbnail and video proxy) explicitly refuse them.

What stays outside the scope of this import: keywords, flags and clip
comments, and the start timecode — `LibraryMedia` has no field to hold them.
Resolve timelines themselves are read separately, with
`--import-resolve-timeline` (MCP: `import_resolve_timeline`), and the way back
to Resolve goes through `send_to_resolve` (below) or
`--export-resolve-xml` (MCP: `export_resolve_xml`).

The XML export requires a new destination: an existing file or link is refused
with `OutputExists`, the project and the source clips included. Choose a new
name for each delivery. The complete XML is prepared next to the destination,
then published without replacement (FI6).

## Sending an edit back to Resolve, whole

> PARITE-2026-11, PA16. Every API call the send-back relies on has been
> measured in Resolve Studio 21.1.0.17, in a throwaway project, on source
> clips with their frame number burned in, then a test edit sent back,
> rendered by Resolve and compared with the engine's export frame by frame
> (the detail is in the batch, PA16). Sending back a real film has not been
> tried.

A single call sends the active timeline into the project open in Resolve, as a
new timeline, then reads it back:

```sh
./build/cutmachine --send-resolve ./Film.cutmachine-project/project.cutmachine.json \
  --name 'Film v3'
```

MCP: `send_to_resolve` (`name`, `grade`, `resolve_node`, `dry_run`); in the
window, **Fichier ▸ Envoyer dans Resolve** (File ▸ Send to Resolve), which
sends the timeline on screen without asking for a name and reports the result
in the status line (a sheet when something was not sent or the send is
refused). It needs Resolve **Studio** 21.1 or later, open on the project, with
the edit's source clips in its Media Pool (`--import-resolve` links them there
in the other direction). The engine writes the descriptor (schema
`cutmachine.resolve-timeline.v3`) in
`~/Library/Application Support/CUTMACHINE/resolve-send/` (or
`CUTMACHINE_RESOLVE_SEND_DIR`), then launches
`sidecar/resolve_bridge.py --send` and returns its report. The bridge is found
through `CUTMACHINE_RESOLVE_BRIDGE`, then next to the executable, then in the
build tree; its Python through `CUTMACHINE_RESOLVE_PYTHON`, otherwise
`/usr/bin/python3`.

**What goes through**, by the Resolve 21.1 API:

| In CUTMACHINE | In Resolve |
|---|---|
| video clip | picture only (`mediaType` 1), on its V track |
| audio clip | sound only (`mediaType` 2), on its A track; a track that carries only the source clips' sound is **mono**, music stays stereo. A mono track is centred at −3 dB in Resolve's stereo bus: the voice comes out 2.5 to 2.8 dB lower than in the engine (measured) |
| picture and sound of the same split | linked (`SetClipsLinked`) |
| constant speed | `SetSpeed`: the clip is placed at its length at speed 1, then slowed or sped up; position and duration do not move |
| freeze frame | `SetSpeed` at 0, which freezes the **last** frame of the placed range |
| dissolve, audio cross-fade | `AddTransition` ("Cross Dissolve", "Cross Fade +3 dB") on the end of the outgoing clip, centred, to the left or right of the cut |
| fade in from or out to black, dip to black | opacity fades of the clips (`SetFades`), without handle, like the engine's veil |
| framing | Zoom, Pan, Tilt, computed for "Scale entire image to fit" **and** for "Scale full frame with crop"; the bridge takes the project's |
| opacity, disabled clip | `Opacity`, `SetClipEnabled` |
| gain and fades of an audio clip | `AudioVolume`, `SetFades` |
| hidden, muted track, or track outside the solo | disabled track |
| markers | `AddMarker`, nearest colour among Resolve's sixteen; two markers on the same frame make only one, their names joined |
| caption cards of the first caption track | subtitle track, placed **first** |
| grade, with `grade: "cdl"` | `SetCDL` on the node `resolve_node`, the CDL that `export_cdl` folds: correct only if that node grades the values that the engine grades |

**What does not go through** is not refused: it is named in `not_sent`, clip by
clip, and the rest of the edit goes. Keyframes (no API sets any: the clip
keeps the framing and opacity of its first frame; animated framing goes
through `export_resolve_xml`), a crop, a blur, a mask, a speed ramp (sent at
its average speed), a motion clip (its place stays empty:
`render_motion_video` makes a plate to place from it), a second caption track,
the caption cards' style (it is grafted through the `.drt`, see the skill),
the sequence look, the grade unless requested, a dip to a colour other than
black.

**What is refused before anything is created in Resolve**: a source clip
missing from the Media Pool, a project at another frame rate, a framed edit in
a project that neither fits nor fills, a boundary that does not fall on a
whole frame, a freeze frame too close to the start of its source clip, a
slow-motion too close to its end (both would read outside the source clip at
speed 1). A timeline of a different size from the project takes its own
settings.

**The read-back.** The bridge reads back each clip (position, duration, speed,
activation), each transition (its boundaries), the markers, the subtitles, the
length of the timeline. A discrepancy undoes nothing: it is listed in
`mismatches` and `verified` is false. `GetSourceStartFrame` is not read back:
it is off by one frame on half the clips (measured; the displayed frame is
right).

**The name.** Without `name`, the timeline takes the name of the sequence, or
the first "name 2", "name 3"… that the Resolve project does not yet have: a
second send of the same edit does not collide with the first. A `name` that is
given and already taken is refused, like a name that contains ":". A dialog
open in Resolve silences it: the send gives up after ten minutes
(`ResolveTimeout`), and the timeline that was started stays in Resolve.

**The time.** The report carries `timings_ms`, step by step, and `elapsed_ms`,
the whole. Measured on an edit the size of an interview (220 clips, 100
subtitles, 10 markers): 1.7 to 1.9 s, of which 0.8 s for Resolve's
`AppendToTimeline`, 0.25 to 0.34 s for the timeline and its subtitles, 0.23
to 0.26 s for the clip settings, 0.17 s for the markers, 0.10 to 0.14 s for
the read-back, 7 ms for the connection.

`--export-resolve-timeline` writes the same descriptor without sending
anything; so does `dry_run`, returning what would go and what would stay.

## Reading a timeline back, and what was graded in it

> GRADE-RESOLVE-2026-10, DEC15. The reading has been verified on copies of
> five databases, never with Resolve open on them.

A Resolve timeline is read back into an empty timeline of the open project,
from the manifest that the bridge writes (`resolve_bridge.py --read-timeline
NAME`, Resolve Studio open on the project):

```sh
./build/cutmachine --import-resolve-timeline \
  ./Film.cutmachine-project/project.cutmachine.json ./timeline.json \
  --resolve-database '<…>/Resolve Projects/Users/guest/Projects/Film/Project.db'
```

MCP: `import_resolve_timeline` with `manifest_path`, `timeline_id` and
`resolve_database`, absolute paths. Without `resolve_database`, the response
is the earlier one: clips, tracks, fades, slowed clips, `unpublished`.

**The grade.** With the project's database (`Project.db`, in Resolve's project
library), the response also carries `grades`: what was graded in Resolve, clip
by clip. A grade that the `color.resolve_primaries` stage renders entirely is
also written on its clip (GR16), in the journal of the read-back (below); the
others remain a reading, and the response says why. For those, the agent
brings the image back with the engine's grade: it knows what was done, and
what the reading could not read.

- **The database is never opened.** The engine copies it (with its `-wal` and
  its `-shm` when they exist) into a temporary folder, reads the copy, then
  deletes it. Resolve may hold the database meanwhile; if a write lands during
  the copy, the copy may be unreadable: save the project in Resolve, then run
  again.
- **The matching.** Each video clip of the import is looked up in the
  database's timeline that carries the manifest's name: same track (V1 is V1),
  same place (the timeline's origin plus its start, in as many frames), same
  file. A clip with no counterpart is named in `unmatched` with its reason
  (often a database older than the timeline), and the edit is read back
  anyway. When the database carries several timelines of that name, the one
  that carries the whole edit is used; if none or several carry it, the call
  is refused and nothing is written (rename the timeline in Resolve).
- **What is read.** For each graded clip (`clips`, with the `clip_id` it
  receives here) and for the timeline itself (`timeline_grade`, `null` when it
  has none): the active version, its nodes in the order they apply (`serial`),
  and in each node the primaries that are set: `lift`, `gamma`, `gain` (r, g,
  b, master), `offset` (r, g, b), `contrast`, `pivot`, `saturation`, `hue`,
  `temperature`, `tint`, `midtone_detail`, `color_boost`, `shadows`,
  `highlights`, `lum_mix`, the Log wheels (`log.*`). A clip with no correction
  is only counted (`ungraded`).
- **The values are the ones Resolve stores**, not converted: neither those of
  its panel, nor those of the engine's wheels. What each one does, on the
  values where the node works (DaVinci Intermediate in a colour-managed v2
  project), has been measured in Resolve 21.1 (GR15), to within 0.05 10-bit
  code:
  - `gain` multiplies, and `lift` sets the black while holding the white.
    Together, the output is a + b·x, with a = lift·gain / (1 − lift +
    lift·gain) and b = gain·(1 − lift) / (1 − lift + lift·gain).
  - `offset` adds 0.1832 times its value, after the gain.
  - `gamma` raises to the power 1 / (1 + γ) if γ ≥ 0, and 1 − γ otherwise,
    after the offset.
  - The `master` values act on the luma (Rec.709 weights) at the weight of
    `lum_mix`: 1, or absent, is the panel's 100. At the same weight, a colour
    that moves one channel keeps the image's luma.
  - `saturation` is a factor around the luma, at the weights 0.21 / 0.70 /
    0.09 (50 on the panel, the unit, is 1).
  - `temperature` and `tint` are a 3×3 matrix that keeps the luma of a grey.
  - `contrast` (2 at most) applies after the gamma, around `pivot` (0.435 when
    absent). Its curve depends on two project settings: the S curve and the
    "color space aware" tools.
    - Under 1, or without the S curve, it is a straight line around the pivot:
      slope c below 1, 1 / (2 − c) above.
    - Above 1, with the S curve (the default setting), the ends stay at 0 and
      at 1, and no formula gives it.
    - With the tools on, the reference project's setting, the contrast acts on
      the HLG code of the light divided by 12, and it has its own Y share.

  On the panel, the lift is displayed at half its stored value, the gamma at a
  quarter, and the offset at 25 plus 25 times its value (read off the panel).
  The complete law, and what does not go through in the engine's wheels, are
  in GRADE-RESOLVE-2026-10 (GR15).
- **What is not read is named** in `unread`, node by node (`node` 0 for the
  whole grade): an OFX plug-in (with its identifier), a window, a qualifier, a
  key, curves, a LUT (with its path), a Shot Match, an animated or tracked
  setting, a disabled corrector, a compound node, a wiring that is not a
  simple chain, a setting outside the nodes, a group's grade, a remote version
  graded too (which of the two the clip shows cannot be read), and any
  parameter with an unknown number. Named does not mean active: the reading
  cannot tell whether a window is on.
- **The refusals.** A database that cannot be found (`IoError`), is unreadable
  or of another schema (`ParseError`), or does not have the timeline
  (`ValidationFailed`), refuses the call before any write: it asked for both.

**Rendering a node like Resolve** (GR16). A clip's `color.resolve_primaries`
stage takes a node's primaries as Resolve stores them. They are the same names
as `grades` (`lift_r` … `lift_master`, `gamma_*`, `gain_*`, `offset_r`,
`offset_g`, `offset_b`, `saturation`, `lum_mix`, `contrast`, `pivot`,
`temperature`, `tint`), as exact fractions, and the stage renders them by the
law above. Three more parameters give the project's settings, at 0 or 1: those
the contrast depends on, `color_space_aware` (0 when absent, as in Resolve)
and `contrast_s_curve` (1 when absent), and `acescct` (0 when absent), which
says that a node was graded in an ACEScct project.

Contrast above 1, with the S curve, has no formula: the engine carries a table
measured in Resolve 21.1. It covers 33 contrasts from 1 to 1.8 and 33 pivots,
and the engine interpolates between them. The monitor receives each node's
curve as 65 values.

- **Where it acts.** Under colour management, the value goes to linear light,
  into DaVinci Wide Gamut (Bradford from the AP1 white), then into DaVinci
  Intermediate, where Resolve grades; the law applies, and the value comes
  back. Without colour management, the law applies to the displayed signal, as
  in an unmanaged Resolve project.
- **What is proven.** The first measurement counts 143 cases, the one for
  temperature and tint 152 more, 125 lattice points each
  (`tests/fixtures/resolve_primaries/`). At the worst of Resolve's cube, the
  engine lands at:
  - 0.03 10-bit code without contrast;
  - 0.044 code with a temperature or a tint, alone, together, with each other
    primary, and on 12 randomly drawn nodes;
  - 0.75 code with the contrast, "color space aware" tools on;
  - 1.14 codes with the tools off, and 1.73 without the S curve;
  - 3.5 codes with a temperature and a contrast under 1 at Lum Mix 100, where
    the contrast alone already misses Resolve's cube by 2.7 codes;
  - in ACEScct (211 cases), 0.03 code for the primaries and the saturation,
    full nodes included; up to 1.9 codes with a temperature or a tint, on
    values over 100 in linear light where Resolve rounds (0.035 at the 99th
    percentile); the contrast as in YRGB; with a contrast and a temperature,
    up to 23 codes where the temperature pushes a channel far below 0, where
    Resolve extends the contrast with a different slope from the table (4
    codes elsewhere).

  Between the table's settings, 16 curves measured off the grid land at 0.06
  code for a pivot from 0.25 to 0.75, and up to 4.8 at the extreme pivots. The
  monitor (Metal) and the export (the cube) render the same thing: to within
  2·10⁻⁶ on the displayed signal, and 10⁻³ in ACEScct or for an ACEScct node.

  On real grades, those of 11 clips of a real project, the stage lands at 1.1
  codes at the worst of the cubes Resolve output for those clips, and 0.28 at
  the 99th percentile. `cutmachine_resolve_grade_tests --compare` redoes the
  comparison for another project, from its cubes and a copy of its database.
- **Only one working luminance measured.** With the "color space aware" tools
  on, the timeline's working luminance changes the contrast, by several
  hundred codes (258 for a contrast of 1.2 in SDR 100). The table and the HLG
  domain hold for HDR 1000, that of the reference project and of 266 of the
  284 projects surveyed. With the tools off, the luminance changes nothing.
- **The bounds**, which hold all the grades surveyed:
  - lift in [−2, 2], gamma in [−4, 4], gain in [0, 16];
  - offset in [−8, 8], saturation in [0, 2], `lum_mix` and `pivot` in [0, 1];
  - `temperature` in [−4000, 4000] and `tint` in [−100, 100], the stops of
    Resolve's panel;
  - `contrast` in [0, 9/5] with the S curve, where the table stops, and in
    [0, 2[ without it;
  - a lift and a gain that would put the black above the white
    (1 − lift + lift·gain ≤ 0) are refused.
- **Temperature and tint.** They act first, on the node's values, before the
  lift, the gamma and the gain, as a 3×3 matrix. It is a Bradford adaptation,
  in the BT.709 primaries, from the 6,500 K white to the white of 6,500 K
  minus the temperature, shifted by the tint divided by 10,000 along its
  isotherm line (Robertson lines). Resolve comes back from the cone domain by
  the inverse of Bradford rounded to four decimals, and keeps the shell of the
  Wyszecki and Stiles table at 325 mired: the engine does the same. The rest
  of the law reads the values balanced this way, Y share included.
- **In ACEScct** (`acescct` at 1). The node grades ACEScct codes in AP1: under
  colour management, the stage takes the working value there (as is when the
  engine grades in ACEScct), and not DaVinci Intermediate. The law changes in
  three points:
  - a negative value passes through the gamma unchanged, instead of keeping
    its sign under the power;
  - saturation turns around the Rec.709 luminance (0.2126, 0.7152, 0.0722),
    and not the DaVinci YRGB weights;
  - temperature and tint: the same matrix, on ACES2065-1 (AP0) light raised to
    the power 1/2.4, a negative value keeping its sign; a grey moves by a
    constant in log.

  The contrast follows the law of the tools off, and the working luminance
  changes nothing there. With the tools on, the contrast follows another law,
  never measured: the stage refuses it by name.
- **Where it is placed.** At the head of the canonical order: what is added by
  hand afterwards comes after the imported node, as a second node.
- **The format.** A document that carries the stage goes to version 22. An
  older binary, which would have skipped it and rendered the clip ungraded,
  then refuses the file.
- **`export_cdl`** refuses it by name: the Y share and the passage through
  DaVinci Intermediate have no CDL form.

**Written by the read-back.** With `resolve_database`, each clip whose grade
renders entirely receives its nodes, one `color.resolve_primaries` per node
that is on, in the order of the chain. They come at the head of the stack,
through a `SetClipEffects` in the read-back's journal. The stored values are
written as exact fractions, those of their shortest decimal (1.106 stored
gives 553/500).

The project's settings are read from the same copy of the database
(`SM_Config`): the colour science, the "color space aware" tools, the S curve
and the working luminance. A node with a contrast set is written with them.
The response gives them in `grades.project_settings` (`read`,
`color_space_aware`, `contrast_s_curve`, `working_luminance`,
`color_science`, and `unread` when they cannot be read).

The colour science decides what is rendered:
- in DaVinci YRGB and in DaVinci YRGB Color Managed, everything the stage
  takes; their cubes are identical to the last digit;
- in ACEScct, everything the stage takes, each stage carrying `acescct` at 1,
  except a contrast with the "color space aware" tools on, which no ACEScct
  project turns on;
- elsewhere (Color Managed v1, ACEScc, never measured), no node.

Across the 293 projects surveyed: 101 in Color Managed, 97 in ACEScct and 95
in YRGB.

A grade is written whole or not at all: a half rendering would be an image
that Resolve never showed. Nothing is written when:
- the reading named something it does not read (`unread`);
- a node carries a setting that the stage does not yet render (hue rotation,
  Log wheels, Color Boost, Midtone Detail, highlights or shadows, away from
  their neutral);
- a node carries a contrast, a saturation, a temperature or a tint, and the
  project's settings cannot be read, or its colour science is not one of those
  where that setting was measured;
- the project's colour science has never been measured;
- a node carries a contrast, "color space aware" tools on, and the working
  luminance is not HDR 1000;
- a value falls outside the stage's bounds.

On a copy of a real project, 214 of the 263 graded clips are written (202
before temperature and tint).

In `grades.clips`, `written_stages` counts the stages written on the clip,
`unrendered` says in French why nothing was, and `written` counts the clips
written. The grade of the timeline itself is never written
(`timeline_unrendered`): a stack belongs to a clip.

The Inspector (Inspecteur) has no control for the stage, and **Réinitialiser
l'étalonnage** (Reset grade) removes it like the others.

From the Agent panel and for an agent relayed to the window, the import goes
through the same steps, on the window's document and journal, with the same
response (PHASES-2026-08 F2.4; not seen on screen).
