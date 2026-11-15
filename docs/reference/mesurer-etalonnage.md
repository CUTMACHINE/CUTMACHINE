# Measuring the grade

`--measure-color` (MCP: `measure_color`) gives the numbers a colourist reads
on their scopes, for one frame of each requested clip (`Start`, `Middle` or
`End`), as the export delivers that clip alone: grade, sequence look and
output transform included, without opacity, fade, title card or clip above.
Only the part of the source that the sequence shows is measured.

- `display`: luminance at the 2nd, 50th and 98th percentiles as a percentage,
  share of clipped and crushed pixels, mean saturation and at the 95th
  percentile (unit of `signalstats`' `SATAVG`), and balance of shadows,
  midtones and highlights (`warmth`, `green_magenta`);
- `scene`, under colour management: the working space's linear luminance in
  stops relative to 18 % grey, the unit of `color.exposure` under ACEScct;
- `region`, if given (exact fractions of the frame): the same numbers on the
  area, plus its hue on the vectorscope and its distance from the skin line.

To match a clip to a reference clip, `--match-color` (MCP: `match_color`)
computes the exposure, balance and saturation correction, and returns the
`set_clip_effects` call that would apply it, without modifying anything: it
adjusts the settings the clip already has and adds only the ones that are
missing. The response gives the numbers before, after and of the reference,
and the remaining difference. The step is passed as is to
`prepare_edit_plan`.

For all the clips of a timeline at once (or those of `clip_ids`), under colour
management, two MCP tools propose without applying anything (AGENT-2026-09
A16, A20 to A22):

- `propose_exposure` measures each clip like `measure_color` and, when its
  scene median differs by more than `tolerance_stops` (0.5 by default) from
  `target_stops` (0, mid grey), returns under `steps` a `set_clip_effects`
  step that moves only its exposure; `clips` says for each one what was
  measured and why it has a step or not (`proposed`, `within_tolerance`,
  `no_scene_measure`). The steps are passed to `prepare_edit_plan`, or to
  `set_clip_effects` one by one.
- `propose_grade`, preferable on an interview, proposes exposure and balance,
  read where a colourist reads them: on the largest face in the frame
  (`anchor: "face"`, the default, target +0.7 stop, +1.5 when the sequence
  outputs in HLG), on a named white or grey area (`neutral`), or on the whole
  frame, exposure only (`median`). A clip with no face found takes the
  exposure that the faces found left in the edit, and no balance. A lift stops
  where the frame's whites would go (`highlight_ceiling_stops`, reason
  `limited_by_highlights`); in HLG, without that named ceiling, where the
  rendered image would clip more than before (`highlight_ceiling:
  display_clipping`, GR15). `apply: true` applies all the proposals; a scene
  meant to be dark or bright is left out of `clip_ids`. The face found is the
  largest thing that looks like one: on a subject wearing make-up or a mask,
  it is the make-up.

After applying, measure again.

`color.temperature` is a real colour temperature: the light that lit the
scene, adapted (Bradford, in linear light) to the white balance the camera
shot at. Ingest reads this balance in Sony files (`rtmd` track);
`set_media_white_balance` sets it by hand or reads it again (`from_camera`)
for media ingested earlier. At the shooting balance, the temperature is
neutral.

A media item's shooting balance and colour input belong to the project. They
are read only once from the file, when the media enters the media library, and
by the same probe whatever the path: import in the window, `--ingest` and
`ingest_media`, `--import-resolve`. A given source clip therefore enters read
the same way by any of them. After that, they change only on request
(`set_media_white_balance`, `set_media_color_input`): a reconnection, or an
ingest that completes an entry of an older project, keeps them. A
`color.temperature` is set against its media's balance: changing the balance
of an already graded media item changes the image of all its clips, and
`propose_grade` must then be redone. A media item ingested before the balance
was read has an unknown balance, and 6500 K stands in for it.

A primary correction is exchanged in the ASC CDL format (slope, offset and
power per channel, then saturation). `--export-cdl` (MCP: `export_cdl`) folds
a clip's exposure, contrast, hue, wheels and saturation into one CDL, writes a
`.cc`, `.ccc` or `.cdl` through OpenColorIO and gives the table that
`TimelineItem.SetCDL` takes in DaVinci Resolve's scripting; a stage with no
CDL form (temperature, curves...) refuses the clip and names it.
`--apply-cdl` (MCP: `apply_cdl`) does the reverse: the CDL, read from a file
or given by Resolve's strings, replaces the clips' primary (in `color.wheels`
and `color.saturation`). The space the CDL was made in is declared
(`--space acescct`...) and must be the one the clips are graded in.

Tested in Resolve Studio 21.1 (GR12):
in an ACEScct project, the table set by `SetCDL` gives the same image as
here, to within 0.03 10-bit code. In a DaVinci YRGB project, managed or not,
Resolve computes the CDL's saturation with the weights 0.21 / 0.70 / 0.09
instead of Rec.709's: a saturation other than 1 does not render the same image
there (1.8 codes for 1.1, 9 codes for 0.5). The response of `export_cdl` and
of `--export-cdl` says so (DEC30, GR12): each clip whose saturation is not 1
carries `resolve_yrgb_gap_codes`, the largest gap it will have in a YRGB
project, in 10-bit codes (|1 − s| × 0.0178 × 1023, rounded to the tenth), and
the response carries a `warning` and the list `warning_clip_ids`. The engine
does not know which colour science the Resolve project works in: the warning
comes for any saturation other than 1, whatever the CDL's space. Resolve has
no `GetCDL`: the strings are copied from what was set, not from Resolve.

A grade is copied from one clip to others (GR11): `paste_clip_grade`
(`from_clip_id`, `clip_ids`), or in the **Couleur** (Color) window, **Copier
l'étalonnage du plan** (Copy clip grade) then **Coller l'étalonnage sur la
sélection** (Paste grade onto selection). The source clip's whole stack
replaces that of each clip, in one undo; a motion clip, an audio clip or a
graphic under colour management are left alone and named.

To see what is measured, `read_frame` (MCP) takes `graded: true` with a
`clip_id`: the graded clip's frame, look included, in Record's SDR view.
Edit sheets take it too (DEC29): `contact_sheet` and `cut_sheet` with
`graded: true` (`--contact-sheet` and `--cut-sheet` with `--graded`) show each
cell the way graded `read_frame` shows its clip, through the same chain, pixel
for pixel; a graded sheet of junctions judges how well colours match from one
clip to the next. `media_sheet` stays ungraded: a source clip has no grade of
its own: only a timeline clip sets one.

The measurement judges nothing and does not modify the project. The source is
decoded the way the monitor decodes it, not by swscale's YCbCr conversion,
which reads codes 0.385 % too low (see
GRADE-2026-09).
