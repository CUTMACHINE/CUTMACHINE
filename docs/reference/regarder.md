# Looking at a shot

> The observation that opens this section and what the images show are not
> verified by reading.

Measuring is not enough, and the gap showed on a real edit: a shot rated
`Sharp` / `Steady` — so perfectly clean — was unusable in a cut because it
showed someone **talking**. The viewer saw a mouth articulating something other
than what they heard. No threshold on the Laplacian variance catches that; an
editor catches it at a glance.

`read_frame` (MCP) therefore returns the image itself, as a JPEG, next to its
text response. Either `clip_id` with a `position` (`Start`, `Middle`, `End` —
never a timecode, the engine resolves the frame), or `media_id` with an exact
`source_in` to inspect a source clip that is not edited yet. A source clip
missing from its path is refused with `MediaMissing`, with the path searched. In
strict mode, `read_frame` on a `media_id` does not need a `timeline_id`.

The division of labour is the same as everywhere else: what is **provable** is
decided by code ([`ShotQuality.h`](../../src/ShotQuality.h)), what needs a look
gets an image ([`FrameCapture.h`](../../src/FrameCapture.h)). Nothing in
`FrameCapture` decides, it renders.

On the transport side, the image crosses MCP as an `image` block next to the
`text` block, and the built-in chat forwards it to the model in the
`tool_result`. It is passed only to providers whose API accepts an image in a
tool result; elsewhere the text goes through and the image is dropped, rather
than replaced by a description that the model would take for an observation.

# Looking at the edit as sheets

A single image does not allow judging the rhythm or continuity of an edit.
`--contact-sheet` (MCP: `contact_sheet`) therefore renders the midpoints of the
clips that are actually visible, after occlusion by the upper video tracks.
`--cut-sheet` (MCP: `cut_sheet`) places side by side, for each selected visible
junction, the last frame before the cut and the first frame after.

Both commands produce a JPEG and a JSON that ties each cell to its `clip_id`,
its `source_id`, its timeline position and its `source_in`, all expressed as
`RationalTime`. They go through the SDR view of the document's colour pipeline:
a log source clip is shown as in the monitor, not as a washed-out image taken
straight from the file. No project data is modified.

By default, a cell shows the clip's source, without its grade or the sequence
look. With `--graded` (MCP: `graded: true`), each cell is the clip as
`read_frame` with `graded: true` shows it: graded, look included, in Record's
SDR view, computed by the engine through the same chain (DEC29). The JSON then
carries `"graded":true`. A graded sheet decodes one image per cell, where the
ordinary sheet reads them all in a single call to FFmpeg: it is slower.
`media_sheet` does not take it: a source clip has no grade until a clip applies
one.

By default, a sheet contains at most 24 images. If the edit offers more, the
engine samples uniformly from start to end; a sheet of junctions never breaks a
pair. `--max-images` adjusts the bound in the CLI, and `max_images` adjusts it
in the MCP (1 to 64 for `contact_sheet`, 2 to 64 for `cut_sheet`).

A timeline with no visible clip has nothing to show in a contact sheet, nor a
timeline with no visible junction (a single clip, clips separated by gaps, a
title placed on a clip) in a sheet of junctions: the command and the tool
refuse with `ValidationFailed`, with a sentence that begins with
`nothing to show:` and says why, and write nothing (PRD39).

A cell has the shape of the sequence (320 × 180 in 16:9, 180 × 320 in 9:16): no
black bar, and a source clip of another shape is fitted there as in the
monitor.

For the **dérush** (the review of source clips), before any editing,
`--media-sheet` (MCP: `media_sheet`) looks at source clips rather than a
timeline: a single source clip gives `max_images` images spread over the take,
several source clips give the middle image of each; at most 24 by default, like
the other sheets, by the command as by the tool. The cells take the shape of
the first source clip, and each cell gives its `source_id`, its `source_in`, and
its `shot_id`: the shot description that covers it, or `null` if no one has
described it yet (PLANS-2026-10, below).

# Describing shots once, searching them afterwards

> PLANS-2026-10, PL1 to PL4. Proven by the tests under Linux; not yet used on a
> real project.

The speech of the source clips is indexed (timed words, blocks); the picture is
indexed by shot descriptions, written once by whoever looked — a model or a
person — and kept in the project. A description covers a range of a source
clip, in exact time (`source_in`, `duration`, in the source clip's time, like a
timeline clip), and states:

| Field | Values |
|---|---|
| `size` (closed) | `extreme_wide` (very wide), `wide` (wide, establishing), `full` (medium full, full length), `medium` (cowboy, waist), `medium_close` (chest close-up), `close` (close-up, tight), `extreme_close` (extreme close-up, very tight), `insert` (object, detail) |
| `movement` (closed) | `static`, `handheld` (shoulder), `pan`, `tilt`, `zoom_in`, `zoom_out`, `push_in` and `pull_out` (dolly in, dolly out), `tracking` (tracking, following), `crane` (crane), `aerial` (drone) |
| `quality` (closed) | `good` by default; `usable` and `poor` require a `reserve` that says why |
| `subjects`, `keywords` (free) | 8 subjects of 80 bytes, 16 keywords of 40 at most, with no duplicates ignoring case and accents |
| `action`, `place`, `reserve` (free) | 300, 120 and 200 bytes at most |
| `author_kind`, `author`, `described_at` | `model` or `person`, its name, the UTC time `AAAA-MM-JJTHH:MM:SSZ` |

The ranges of a source clip do not overlap, stay within the source clip, and a
source clip with no picture has none. Every refusal names the field.

**Writing**: `describe_shots` (MCP, or `--call-tool`) takes `media_id`, `shots`
(one entry per range; `shot_id` to replace an existing description),
`remove_shot_ids`, `author` (required with `shots`), `author_kind` (`model` by
default) and `described_at` (now by default). A range that overlaps a
description that is neither replaced nor removed is refused, naming both. A
call is one operation, `SetMediaShots` (the source clip's whole list, the
inverse is the previous one), and a single `undo`. On the command line,
`--apply-op` takes it as the journal writes it.

**Searching**: `search_shots` filters on the closed fields (`sizes`,
`movements`, `qualities`, `author_kind`, `min_duration`, `media_ids`; any one
of the values of a filter, all the filters at once) and on words (`text`): each
must appear in the subjects, action, place, keywords or reserve, ignoring case
and French accents, as a substring ("velo" finds "vélos"). The response follows
the media pool's order then time, never a ranking; `total` counts everything,
`limit` (50 by default, 500 at most) bounds the list. Each shot gives its exact
range to cut and `look`: the arguments of `read_frame` (the middle image of the
range) and of `media_sheet` to see it.

**What is not described**: `describe` gives `described_shots` for each source
clip that has a picture (0: to be described), `media_sheet` the `shot_id` of
each cell, `list_stable_windows` the `shot_ids` that cover each window and
`undescribed_windows`, `search_shots` the `undescribed_media` of its scope.

The descriptions live in the document (version 19 as soon as a source clip has
one) and not in a cache next to the media like the transcription or the shot
quality: they cannot be recomputed for free. An earlier binary therefore
refuses the project instead of losing them.

# Looking at a junction as it is delivered

> From the binary's catalogue (IA-2026-09 IA4), and played at the CLI on a test
> project.

A sheet shows what the source clips contain at the bounds of the clips; it
shows neither the fade, nor the opacity, nor the animated framing, nor the
title card, nor the sound. `inspect_cut` (MCP, or `--call-tool`) renders a
short window around a junction through the export's own graph — all the layers,
opacities, grades, framings and keys, transitions, captions, look and the mix
with its gains and fades — then rereads it: what it shows is the film as
delivered at that point.

The junction is named by `clip_id` (its head, or its tail with `at_tail`) or by
a timeline `position`; `before` and `after` say how much of the edit to render
on each side (1 s by default, 5 s at most, in whole timeline frames). The
response gives:

- the chosen images (`images`, 2 to 12, 6 by default) in a JPEG band, with
  their exact frames and times; the last frame before the cut and the first at
  the cut are always in it;
- the mix level frame by frame (dBFS) and on each side of the cut;
- the words that the edit must say in the window (`expected_words`); with no
  cached transcription for the sound tracks, the list is empty and
  `expected_words_unavailable` says why;
- `alerts` to examine: a word that the cut crosses and, with `transcribe` and a
  configured local Whisper model (refused otherwise), the words expected but not
  heard in a fresh reading of the window's mix, or heard but not expected. An
  alert is not a verdict: a quiet line may be audible to a viewer and not to the
  model.

An HLG delivery is shown through the SDR view, as Record shows it. Nothing in
the project is modified.

The window is rendered at twice the size of the largest image drawn from it (a
band image or an analysis image), without ever exceeding that of the sequence:
the graph composites captions, motion, blur and framings for the size it
delivers, as for a 1080 export of a 4K sequence. Rendering at full definition,
in 32-bit floating point then lossless, would cost 5.5 s per junction for a
review of 36 cuts on a 1080×1920 short, while its images are 240 pixels.

With `band: true` (RACCORDS-2026-10 RC2), the images are rendered as a
**band**: those before the cut on one line, a red bar, those from the cut on,
and below them the waveform of the mix of the whole window, the cut in red and
a grey line where each image falls.

# Reviewing the junctions

> RACCORDS-2026-10 RC1, RC2. Played under Linux on synthetic media only; neither
> on a Mac, nor on a real film.

An agent does not watch the film and does not listen to it. `review_cuts` (MCP;
CLI: `--review-cuts <projet> <image.jpg> [--timeline <id>] [--frame <n>]...
[--max-bands <n>]`) does it for the agent at every **junction**: a frame where,
on a visible video track or an audible sound track, a clip touches the next.
Each window is rendered like that of `inspect_cut`, through the export's graph
(1 s on each side by default, `side` up to 5 s), then measured:

| Signal | Measure | Alert |
|---|---|---|
| Ambience | 10th percentile of the mix's 10 ms levels, on each side | `ambience_jump`: ≥ 6 dB apart |
| Voice | 90th percentile, when it exceeds the ambience by 10 dB on both sides | `voice_jump`: ≥ 6 dB apart |
| Edge | the 30 ms that touch the cut, against the ambience on their side | `sound_cut_live`: ≥ 12 dB above, and ≥ −50 dBFS |
| Words | the words of the transcriptions that the cut crosses | `word_crosses_cut` |
| Picture | last frame before / first after: mean difference and histogram, in ‰ | `jump_cut`: ≤ 80 ‰ and ≤ 150 ‰ |
| Motion | the two frames on each side | `motion_interrupted`: ≥ 30 ‰ before, ≤ 8 ‰ after, same scene (≤ 250 ‰) |

A **straight cut** (the next clip reads its source where the previous one
stops, at speed 1) is measured and raises nothing. The sound alerts count only
where the sound cuts, the picture alerts where the picture cuts. The report
declares its thresholds (`thresholds`); they cannot be set at call time.
**An alert is a measurement to examine, not a verdict**: a jump cut or a dry
cut in the ambience may be intended, and the absence of an alert does not say
that a junction is good.

By default, all junctions are reviewed (60 at most; the others are listed in
`not_reviewed`); `positions` or `clip_ids` (clip heads) name some of them. The
response attaches a sheet of **bands** (`max_bands`, 6 by default, 12 at most;
`images_per_side`, 3 by default), the junctions that have an alert first, in the
film's order; each junction says its `band`. Each window is a render: count a
few seconds per junction. If the project changes during the review, the tool
refuses rather than mix two edits. Nothing in the project is modified.
