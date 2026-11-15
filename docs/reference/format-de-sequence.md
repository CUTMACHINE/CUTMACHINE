# Sequence format

A new sequence is 1920×1080 at 25 fps. That is almost never the format of the
source clips, and correcting it by hand requires knowing something the files
hide: **a vertical shoot is stored in landscape** with a rotation flag.
Reading `3840×2160` in the metadata and copying it into the sequence gives an
edit lying on its side.

`--propose-sequence` infers the format from the project's source clips,
without modifying anything:

```sh
./build/cutmachine --propose-sequence ./Film.cutmachine-project/project.cutmachine.json
```

```json
{"ok":true,
 "chosen":{"width":2160,"height":3840,"frame_rate":{"num":25,"den":1},"media_count":365},
 "unanimous":false,"media_considered":405,"media_ignored":0,
 "candidates":[{"width":2160,"height":3840,"frame_rate":{"num":25,"den":1},"media_count":365},
               {"width":2160,"height":3840,"frame_rate":{"num":50,"den":1},"media_count":40}]}
```

The rule is the majority, and it is entirely deterministic: on a tie in the
counts the largest frame decides, then the highest frame rate, then the raw
fields. The same project always gives the same answer, whatever the order of
the media library. Minority formats are **reported**, not hidden: a shoot at
mixed frame rates has to show. Media with no usable picture (audio only, a
rotation that is not a right angle) are counted separately rather than forced
into a format.

On the agent side, the MCP tool `conform_sequence` applies that format in one
reversible `UpdateSequenceOperation`, and returns what it chose and what it
set aside. `preview: true` computes without applying. A sequence that already
conforms is reported without being rewritten, so as not to clutter the history
with a null operation.

This is the division of labour of principle 7: the caller names the intent
("make the sequence match the source clips"), the engine computes the numbers.
`update_sequence` remains available for setting an arbitrary format.
