# CMX 3600 EDL and WebVTT

FC7 — FILMCRAFT-2026-10. Ported from
FilmCraft (`crates/interchange/src/edl.rs`, `crates/captions/src/vtt.rs`,
MIT or Apache-2.0, commit `09aaf9b`), in `src/media/Edl.h` and `src/media/Subtitles.h`.

**Verified in Resolve 21.1.0.17**: cuts, dissolves, black, matching by file
name with reel `AX` and M2 half speed, on the regression fixtures and on
synthetic media. The markers path through its specialised menu remains to be
played; the timeline import does not place them.

## The EDLs of a timeline

```sh
./build/cutmachine --export-edl ./Film.cutmachine-project/project.cutmachine.json \
  ./livraison/film.edl --timeline '<timeline-id>' [--start-timecode 01:00:00:00]
```

MCP: `export_edl` (absolute `path`, optional `start_timecode`).

**One EDL per visible video track**, numbered from the bottom track like the
layers of the send to Resolve: `film_V1.edl`, `film_V2.edl`… next to the
given path, which must end in `.edl`. The number is that of the track in the
window: a hidden track, or one with no video clip, keeps its own without
writing a list, and `film_V2.edl` remains track V2 even when V1 is hidden.
All the lists or none: a destination that already exists makes the whole
call fail (`OutputExists`), and nothing is written.

What a list carries:

- cuts, each clip as one event, reel `AX` (the auxiliary source of file
  lists), named by a `* FROM CLIP NAME:` comment that carries the name of its
  file;
- dissolves (`D` and their duration in frames), the outgoing clip read at the
  start of the dissolve, the incoming one in its handles (`* TO CLIP NAME:`);
- the opening from black and the closing to black (`BL`), the black
  `dip_to_color` transitions at the edge of a track;
- a constant speed, in an `M2` line, **only if it can be written exactly**:
  the line writes `ddd.d` frames per second, that is speed × timecode base.
  1/2 at 25 fps gives `012.5`, 2 gives `050.0`, 1/2 at 23.976 gives `012.0`;
  2/3 at 25 fps (16.666…) is refused by name. As the CMX convention has it,
  the source duration of a slowed-down event is that of the recording, and
  the M2 line states the speed.

**Timecodes.** The record side starts from `start_timecode`, `01:00:00:00`
by default (the start of a Resolve timeline). The source side is the time of
the source clip counted from `00:00:00:00`: the document does not know the
camera's timecode (see `kUnknownStartTimecode`, [Resolve](resolve.md)), and
the response says so (`source_timecodes: "media_time_from_zero"`). A source
clip whose camera wrote another start will not conform on timecode as long
as that start is not known. 29.97 and 59.94 are written in drop-frame (`;`
before the frames, `FCM: DROP FRAME`); 23.976 counts on a base of 24, without
drop-frame; any other non-integer rate is refused.

**Refused by name**, because writing it would change the edit: a rate with
no timecode, a clip that does not fall on whole frames, a source clip at a
different rate from the sequence (a list counts source and record in a single
base), a speed with no M2 writing, a freeze frame, a dissolve touching a
speed ramp, a dissolve that comes out of a slowed-down clip (with all the
reels being `AX`, two M2 lines in an event would not say which side they
speak for), a `dip_to_color` between two clips or to a colour other than
black, more than 999 events in a list, a timeline with no video clip.

**Counted and named** in `losses` (`kind`, `id`), with a count per family in
the response: the grade (`grades_not_sent`), the framing
(`framing_not_sent`), the opacity (`opacity_not_sent`), the blur
(`blurs_not_sent`, EFFETS-2026-10 EF3), speed ramps, written at their
average speed — their cut points remain exact, the curve is the loss
(`speed_ramps_flattened`) —, motion clips, inactive multicam angles, sound,
caption and hidden tracks, the sequence LUT (`look_lut_not_sent`) and markers
(`markers_not_sent`: `export_edl_markers` writes them).

## Markers

```sh
./build/cutmachine --export-edl-markers ./Film.cutmachine-project/project.cutmachine.json \
  ./marqueurs.edl [--style loc|resolve] [--timeline '<timeline-id>'] [--start-timecode 01:00:00:00]
./build/cutmachine --import-edl-markers ./Film.cutmachine-project/project.cutmachine.json \
  ./marqueurs-resolve.edl [--timeline '<timeline-id>'] [--start-timecode 01:00:00:00]
```

MCP: `export_edl_markers` and `import_edl_markers`.

One event per marker, and one of two writings:

- `loc` (default): `* LOC: 01:00:05:00 YELLOW  nom`, the eight Avid colours
  (`RED`, `GREEN`, `BLUE`, `CYAN`, `MAGENTA`, `YELLOW`, `WHITE`, `BLACK`). The
  comment has no duration: a span is written there as a point, counted in
  `durations_not_sent`;
- `resolve`: the line that Resolve's marker list writes under each event,
  ` |C:ResolveColorBlue |M:nom |D:50`, with its sixteen colours (Blue, Cyan,
  Green, Yellow, Red, Pink, Purple, Fuchsia, Rose, Lavender, Sky, Mint, Lemon,
  Sand, Cocoa, Cream). `D` is the length in frames: a point is written `D:1`,
  a span keeps its own.

A document colour with no word of its own (the orange `#ff8a3d` of the
agent's questions, for example) is written as the closest one, and its marker
is named in `colors_approximated`. A name that contains a line break, or a
`|` in Resolve's writing (its separator), is refused.

**Reading** accepts both writings in one list, `* LOC:` and `|C:` (attached
to the record timecode of its event), and honours the `FCM:` line and the `;`
of each timecode. The start timecode is read back from the list's count: at
29.97, a list written without drop-frame (`FCM: NON-DROP FRAME`) places its
markers where they belong, not 108 frames further per hour. A marker with no
name takes the one the window gives, "Marqueur" (Marker). Each colour word is
read back into the document through this table, case-insensitive:

| Word | Document colour |
|---|---|
| `YELLOW`, `Yellow` | `#f5c542` (the document default) |
| `BLUE`, `Blue` | `#4da3ff` (the blue of the window's markers) |
| `RED`, `Red` | `#ff4d4d` |
| `GREEN`, `Green` | `#4caf50` |
| `CYAN`, `Cyan` | `#3fd0d4` |
| `MAGENTA`, `Fuchsia` | `#d050c8` |
| `WHITE` | `#ffffff` |
| `BLACK` | `#000000` |
| `Pink` | `#ff7fbf` |
| `Purple` | `#9a5cd0` |
| `Rose` | `#ff9aa8` |
| `Lavender` | `#b9a6e8` |
| `Sky` | `#8fd3ff` |
| `Mint` | `#8fe0b8` |
| `Lemon` | `#f0f07a` |
| `Sand` | `#d2b48c` |
| `Cocoa` | `#8b5a3c` |
| `Cream` | `#f5ecd0` |

**An unknown colour is refused by its line**, never guessed. Markers enter
the `note` category, each as an `AddMarkerOperation`, grouped into **a single
undo entry** (a `BatchOperation` "marqueurs EDL", i.e. "EDL markers"): one
Undo removes the whole import. A marker that the timeline already carries
(same time, same length, same name, same colour) is skipped and counted in
`already_present`: a list imported twice does not double. A marker before the
start timecode, a drop-frame on a base that has none, a `|C:` line under no
event, an event whose record timecode cannot be read in this base, a control
character in a name, a list with no marker are refused, by their line. Beyond
256 markers to add, the import is refused rather than cut into several undo
entries.

## WebVTT

```sh
./build/cutmachine --export-vtt ./Film.cutmachine-project/project.cutmachine.json \
  ./film.vtt --timeline '<timeline-id>' [--word-by-word]
```

The same cues as `--export-srt`, the same response, plus `format: "webvtt"`:
`WEBVTT`, an empty line, then for each cue its number,
`HH:MM:SS.mmm --> HH:MM:SS.mmm` and its text. Same rounding to the nearest
millisecond as SRT, the only one on the path, counted in
`rounded_timestamps`. Text stays text: `&`, `<` and `>` are escaped
(`&amp;`, `&lt;`, `&gt;`), since WebVTT reads them as tags. A text that
contains an empty line is refused, an empty line ending a cue in WebVTT.
SRT export has no MCP tool; neither does WebVTT.

## Verifying in Resolve

Point 1 below goes through the API in a throwaway project. Points 2 and 3
remain to be played through the specialised menus, with a free screen:

1. **A timeline.** `--export-edl` on a timeline of simple cuts, then on a
   timeline with a dissolve, a dip to black and a clip at 1/2. In Resolve:
   *File › Import › Timeline…* (or right-click in the Media Pool,
   *Timelines › Import › AAF/EDL/XML…*), choose `film_V1.edl`, set the
   timeline's frame rate to that of the sequence, and **uncheck**
   *Automatically import source clips into media pool* so that it searches
   the Media Pool. Look: are the clips linked to the right source clips (is
   the file name in `* FROM CLIP NAME:` enough with a reel `AX`)? Do they
   land on the right frames (the source timecode is counted from zero: a
   source clip shot with a camera timecode will stay offline)? Are the
   dissolve, the black and the M2 line read? A slowed-down clip that enters a
   dissolve writes its M2 line under two `AX` lines: does Resolve give it to
   the incoming clip (the timecode of the M2 line is its own)?
2. **Markers to Resolve.** `--export-edl-markers --style resolve`, then in
   Resolve, on the open timeline: right-click on the timeline in the Media
   Pool, *Timelines › Import › Timeline Markers from EDL…*. Repeat with
   `--style loc`. Note which of the two Resolve reads, its colours and the
   length of a span.
3. **Markers from Resolve.** In Resolve, place a few markers of different
   colours, including a span, then right-click on the timeline in the Media
   Pool, *Timelines › Export › Timeline Markers to EDL…*, and
   `--import-edl-markers` on that file: an Undo in CUTMACHINE must remove
   everything.

The menu labels above are those expected from Resolve 21, not labels that
were read off: the first pass verifies them too. What comes out of it is
recorded here.
