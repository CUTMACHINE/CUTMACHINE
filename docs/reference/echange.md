# OTIO and FCPXML

> PA1, PARITE-2026-11.

A timeline is written in the two formats that other editors read:
OpenTimelineIO (`.otio`, a JSON file) and Final Cut Pro XML (`.fcpxml`,
version 1.10). For DaVinci Resolve, the whole edit goes through
[`send_to_resolve`](resolve.md) (PA16), and an animated framing through
`export_resolve_xml`; for a per-track list, use [the EDL](edl.md).

**Nothing these two exports write has been opened in an editor**, nor read
back by the OpenTimelineIO library: the files follow the formats as their
documentation describes them. Each report says so (`verified_in_editor:
false`).

```sh
./build/cutmachine --export-otio ./Film.cutmachine-project/project.cutmachine.json \
  /chemin/film.otio --timeline '<timeline-id>'
./build/cutmachine --export-fcpxml ./Film.cutmachine-project/project.cutmachine.json \
  /chemin/film.fcpxml
```

MCP: `export_otio` and `export_fcpxml` (absolute `path`). The destination file
must be new: an existing file is refused (`OutputExists`), as for the EDL and
the Resolve XML.

## What both carry

- all the video and audio tracks in a single file, with the gaps between the
  clips;
- the constant speed of a clip, and the freeze frame;
- a disabled clip, written disabled; a hidden multicam angle and the clips of
  a hidden or muted track travel disabled too, and are counted
  (`multicam_angles_disabled`, `hidden_tracks_disabled`);
- the timeline's markers;
- the absolute path of each source clip: a path stored relative to the package is
  resolved before writing.

No position is refused. Both formats write a time as a rational: in OTIO a
value at a rate, in FCPXML a fraction of a second. In OTIO, a time that falls
on a whole frame is written in frames (those of the sequence for the timeline,
those of the source clip for the source); a time inside a frame is written in its own
rate and is counted in `off_frame_times`.

## What differs

| | OTIO | FCPXML |
|---|---|---|
| Layout | one `Track.1` per track, video from bottom to top then audio | a `gap` the length of the film, each clip connected onto it; video tracks on `lane` 1, 2…, audio tracks on −1, −2… |
| Speed | `LinearTimeWarp` effect (`time_scalar`); `FreezeFrame` for a freeze | two-point `timeMap`; `start` counted in the clip's time once slowed down |
| Dissolve, cross-fade | `Transition.1` between the two clips, if they touch | not carried |
| Fade to a colour, fade at a track's edge | not carried | not carried |
| Marker colour | the nearest word among OTIO's eleven; the difference is named (`colors_approximated`), the original colour stays in the metadata | not carried |

In FCPXML, a clip on a video track plays only the picture and a clip on an
audio track only the sound (`srcEnable`), as in the document.

## What is left behind

Counted and named in `losses`, per clip, track or sequence: the grade
(`grade`), the framing (`framing`), the opacity (`opacity`), the blur
(`blur`), the gain and fades of an audio clip (`audio_gain`, `audio_fade`), a
speed ramp, written at its average speed (`speed_ramp`), a motion clip, whose
place stays empty (`motion_clip`), a caption track (`caption_track`), the
sequence's look (`look_lut`), and each transition that the format does not
place (`transition`).

Refused, by name: a clip whose source media is not in the media library, a timeline
without clips, a relative path or one with another extension.
