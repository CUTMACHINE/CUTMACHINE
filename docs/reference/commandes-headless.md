# Headless commands

These commands run before any initialisation of AppKit, Metal or media
decoding:

```sh
./build/cutmachine --create-project ./Film.cutmachine-project "Film"
./build/cutmachine --create-project ./ITW.cutmachine-project "ITW" \
  --format 1080x1920@25
./build/cutmachine --after-request \
  '{"schema":"after-request.v2","method":"list_comps","arguments":{}}'
./build/cutmachine --transcribe \
  ./Film.cutmachine-project/project.cutmachine.json '<media-id>[,<media-id>…]' \
  ./models/ggml-large-v3.bin fr --verbatim
./build/cutmachine --transcribe-timeline \
  ./Film.cutmachine-project/project.cutmachine.json \
  --timeline '<timeline-id>' --language fr --verbatim
./build/cutmachine --srt-from-media ./film.mp4 ./film.srt --language fr
./build/cutmachine --align-transcripts \
  ./Film.cutmachine-project/project.cutmachine.json --write
./build/cutmachine --tighten-pauses \
  ./Film.cutmachine-project/project.cutmachine.json '<clip-id>' 320 4
./build/cutmachine --trim-boundary-air \
  ./Film.cutmachine-project/project.cutmachine.json '<clip-id>' 3 300
./build/cutmachine --close-junction-air \
  ./Film.cutmachine-project/project.cutmachine.json \
  '<clip-gauche-id>' '<clip-droit-id>' 3 300
./build/cutmachine --locate-source-frame \
  ./Film.cutmachine-project/project.cutmachine.json '<media-id>' 1412
./build/cutmachine --disfluencies \
  ./Film.cutmachine-project/project.cutmachine.json '<clip-id>'
./build/cutmachine --remove-words \
  ./Film.cutmachine-project/project.cutmachine.json '<clip-id>' \
  '[{"start_word_index":40,"end_word_index":40}]'
./build/cutmachine --shot-quality \
  ./Film.cutmachine-project/project.cutmachine.json '<media-id>'
./build/cutmachine --shot-quality-report \
  ./Film.cutmachine-project/project.cutmachine.json
./build/cutmachine --isolated-frames \
  ./Film.cutmachine-project/project.cutmachine.json --analyze
./build/cutmachine --isolated-frames \
  ./Film.cutmachine-project/project.cutmachine.json --analyze \
  --media '<media-id>' --media '<media-id>'
./build/cutmachine --contact-sheet \
  ./Film.cutmachine-project/project.cutmachine.json ./contacts.jpg \
  --timeline '<timeline-id>' --max-images 24
./build/cutmachine --cut-sheet \
  ./Film.cutmachine-project/project.cutmachine.json ./raccords.jpg \
  --timeline '<timeline-id>' --max-images 24
./build/cutmachine --review-cuts \
  ./Film.cutmachine-project/project.cutmachine.json ./bandes.jpg \
  --timeline '<timeline-id>' --frame 250 --max-bands 6
./build/cutmachine --measure-color \
  ./Film.cutmachine-project/project.cutmachine.json '<clip-id>,<clip-id>' \
  --position Middle --region 2/5,1/10,1/5,1/5
./build/cutmachine --match-color \
  ./Film.cutmachine-project/project.cutmachine.json '<clip-id>' \
  '<reference-clip-id>' --match exposure,balance,saturation
./build/cutmachine --export-cdl \
  ./Film.cutmachine-project/project.cutmachine.json '<clip-id>,<clip-id>' \
  --file ./scene.ccc
./build/cutmachine --apply-cdl \
  ./Film.cutmachine-project/project.cutmachine.json '<clip-id>' \
  --space acescct --file ./scene.ccc --id '<correction-id>'
./build/cutmachine --media-sheet \
  ./Film.cutmachine-project/project.cutmachine.json ./derush.jpg \
  --media '<media-id>' --max-images 12
./build/cutmachine --describe ./Film.cutmachine-project/project.cutmachine.json
./build/cutmachine --apply-op ./Film.cutmachine-project/project.cutmachine.json \
  '{"type":"TrimClip","clip_id":"01K00000000000000000000003","edge":"Tail","delta":{"value":-1,"rate":25},"exact_clip":null}'
./build/cutmachine --apply-op ./Film.cutmachine-project/project.cutmachine.json \
  '{"type":"SetClipAudio","clip_id":"01K00000000000000000000007","gain_db":{"num":10,"den":1},"fade_in":{"value":12,"rate":25},"fade_out":{"value":12,"rate":25}}'
./build/cutmachine --apply-project-op ./Film.cutmachine-project/project.cutmachine.json \
  '{"type":"AddProjectTimeline","name":"Vertical","width":1080,"height":1920,"frame_rate":{"num":25,"den":1},"timeline_id":"","video_track_id":"","audio_track_id":"","exact_project_hex":null}'
./build/cutmachine --apply-project-op ./Film.cutmachine-project/project.cutmachine.json \
  '[{"type":"RenameProjectItem","item_id":"<timeline-id>","name":"Version courte","exact_project_hex":null},{"type":"SetActiveProjectTimeline","timeline_id":"<timeline-id>","exact_project_hex":null}]'
./build/cutmachine --undo-project-op ./Film.cutmachine-project/project.cutmachine.json
./build/cutmachine --redo-project-op ./Film.cutmachine-project/project.cutmachine.json
./build/cutmachine --help
./build/cutmachine --help export
./build/cutmachine --version
./build/cutmachine --check-setup
./build/cutmachine --procedure
./build/cutmachine --procedure monter-une-itw mots-et-bornes.md
./build/cutmachine --tools
./build/cutmachine --call-tool ./Film.cutmachine-project/project.cutmachine.json list_timelines
./build/cutmachine --call-tool ./Film.cutmachine-project/project.cutmachine.json clone_timeline \
  '{"source_timeline_id":"<timeline-id>","name":"Déclinaison courte"}'
./build/cutmachine --ingest ./Film.cutmachine-project/project.cutmachine.json ./rushes --recursive
./build/cutmachine --ingest ./Film.cutmachine-project/project.cutmachine.json ./voix-off.wav
./build/cutmachine --ingest ./Film.cutmachine-project/project.cutmachine.json ./rushes --bin <bin-id>
./build/cutmachine --mcp-serve --port 8765
./build/cutmachine --mcp-serve ./Film.cutmachine-project/project.cutmachine.json --port 8765
./build/cutmachine --mcp-serve --stdio
./build/cutmachine --mcp-serve ./Film.cutmachine-project/project.cutmachine.json --stdio --require-timeline
./build/cutmachine --mcp-serve --stdio --profile motion
./build/cutmachine --tools --profile motion
CUTMACHINE_MCP_TOKEN=<jeton> ./build/cutmachine --mcp-serve ./Film.cutmachine-project/project.cutmachine.json --port 8765
curl -sS http://127.0.0.1:8765/mcp -H 'Content-Type: application/json' \
  -H "Authorization: Bearer $CUTMACHINE_MCP_TOKEN" -d '{"jsonrpc":"2.0","id":1,"method":"tools/list"}'
./build/cutmachine --import-resolve ./Film.cutmachine-project/project.cutmachine.json ./manifest.json
./build/cutmachine --import-resolve-timeline \
  ./Film.cutmachine-project/project.cutmachine.json ./timeline.json \
  --resolve-database '<…>/Projects/Film/Project.db'
./build/cutmachine --export ./Film.cutmachine-project/project.cutmachine.json ./film.mp4
./build/cutmachine --export ./Film.cutmachine-project/project.cutmachine.json ./vertical.mp4 \
  --timeline '<timeline-id>'
./build/cutmachine --export-srt ./Film.cutmachine-project/project.cutmachine.json \
  ./film.srt --timeline '<timeline-id>'
./build/cutmachine --export-srt ./Film.cutmachine-project/project.cutmachine.json \
  ./controle-mots.srt --timeline '<timeline-id>' --word-by-word
./build/cutmachine --export-vtt ./Film.cutmachine-project/project.cutmachine.json \
  ./film.vtt --timeline '<timeline-id>'
./build/cutmachine --export-edl ./Film.cutmachine-project/project.cutmachine.json \
  ./film.edl --timeline '<timeline-id>'
./build/cutmachine --export-edl-markers \
  ./Film.cutmachine-project/project.cutmachine.json ./marqueurs.edl --style resolve
./build/cutmachine --export-otio ./Film.cutmachine-project/project.cutmachine.json \
  ./film.otio --timeline '<timeline-id>'
./build/cutmachine --export-fcpxml ./Film.cutmachine-project/project.cutmachine.json \
  ./film.fcpxml --timeline '<timeline-id>'
./build/cutmachine --import-edl-markers \
  ./Film.cutmachine-project/project.cutmachine.json ./marqueurs-resolve.edl
./build/cutmachine --export-resolve-timeline \
  ./Film.cutmachine-project/project.cutmachine.json --timeline '<timeline-id>'
./build/cutmachine --send-resolve ./Film.cutmachine-project/project.cutmachine.json \
  --timeline '<timeline-id>' --name 'Film v3' --grade cdl
./build/cutmachine --add-multicam-group \
  ./Film.cutmachine-project/project.cutmachine.json '<plan-large>,<plan-serré>' \
  --name Interview --angle-name Large --angle-name Serré --active '<plan-large>'
./build/cutmachine --set-multicam-angle \
  ./Film.cutmachine-project/project.cutmachine.json '<plan-ou-angle>'
./build/cutmachine --set-multicam-angle \
  ./Film.cutmachine-project/project.cutmachine.json none --group '<groupe>'
./build/cutmachine --list-multicam-groups \
  ./Film.cutmachine-project/project.cutmachine.json
./build/cutmachine --remove-multicam-group \
  ./Film.cutmachine-project/project.cutmachine.json '<groupe>'
```

`--create-project` accepts `--format LARGEURxHAUTEUR@CADENCE` (for example
`1080x1920@25` or `3840x2160@24000/1001`) to create the sequence directly in
the delivery format, and returns the identifier of its timeline. This
timeline carries a picture track and a sound track, like a timeline added by
`add_timeline`, and the response names them, each with its alias (`T1`,
`T2`): a first clip is inserted without creating a track.

`cutmachine --help` lists all the commands with their arguments;
`cutmachine --help <commande>` (with or without the two dashes) or
`cutmachine <commande> --help` gives the usage of a single one, as plain text,
exit code 0. A refused command line answers `ValidationFailed`, exit code 2,
motion commands included: an unknown command is named, with the closest one
when it is a typo; a known command says what is wrong (`--create-project is
missing <folder> <name>`, `unknown --describe option '--frob'`, `--export
option '--codec' needs a value`, `--contact-sheet option '--timeline' is
given twice`, `--export-resolve-xml option '--resolve-scaling' does not take
'zoom'`) and gives its usage line, the one from `--help`. `--describe`,
`--shot-quality-report`, `--isolated-frames` and `--locate-source-frame` put
`"ok":true` in front of their report, unchanged; the MCP tools that return
the same reports answer without it (PRD39). A project path where nothing
exists is refused with `IoError` and the path; a folder that exists without
being a package keeps `unsupported project package`. A project written by a
newer CUTMACHINE says so, with the version found and the highest one this
binary reads. `--ingest` answers `ok:true` with `added`, `skipped`, `ignored`
and `errors`; when it could read nothing (no file added, none already known),
it refuses with `IoError`, code 1, with these four fields under `result`. A
folder of which only part fails remains a success. `ingest_media` answers the
same way. An ingest is an operation, `IngestMediaOperation` (DEC32,
PRODUIT-2026-10): the new source clips,
the known source clips reclassified, and the repairs made to known ones
(completed metadata, a mounted source) form a single entry of the active
timeline's journal, which the `undo` tool (or `Cmd+Z` in the app) takes back
and `redo` replays, byte for byte. `--bin <id>` (the `bin_id` argument of
`ingest_media`, ULID or alias; `""` for the root) files what is ingested into
that bin, including an already known source clip, which is moved there;
without it, new source clips go to the root and known ones stay where they
are. A bin the project does not have is refused before any reading
(`UnknownBin` at the CLI, `ValidationFailed` over MCP, which resolves the
identifier as for any tool). Only two repairs stay outside the journal: a
version 1 entry completed by the probe, and a complete entry with no source
that receives one.

`--mcp-serve` opens the local MCP server (loopback only). The project is
optional: launched without a project, the server answers only `create_project`,
`open_project`, `get_procedure`, reading the project archive, the charter tools
of the motion library (`read_motion_kit`, `save_motion_kit`,
`edit_motion_kit`, `restore_motion_kit`, see
[Brand charters](api-edition.md#brand-charters)) and the `after_*`
tools, which address an After project and not the CUTMACHINE project
(MOTION-AFTER-2026-09 MA10); it refuses any other tool with `NoProjectOpen`.
A run can thus start from a folder of source clips without leaving MCP:
`create_project`, then `ingest_media`, which does what `--ingest` does. The
server holds the session lock of the open project, and only one: opening or
creating another project locks and reads the new one first, then releases the
previous one; a refusal (wrong path, project held elsewhere, unreadable
package) leaves the previous one open and says so. `--ingest` takes this same
lock: it is refused as long as the application, a server or `--call-tool`
holds the project, because an ingest written on the side left stale the
project that a server keeps in memory.

`--profile <nom>` chooses the tools the server serves, in stdio as on HTTP
(PROFIL-2026-11). A client loads every listed
tool into its model's context: the whole catalogue weighs about 87,000
tokens. `full`, the default, serves everything; `motion` serves motion,
motion clips and the After bridge in full, plus what is needed around them
(project, timelines, clips, music, looking, export, undo, tasks): 63 tools
out of 210, about 23,000 tokens. A tool outside the profile is refused by
name, `ToolNotInProfile`, which says how to get it; the server's instructions
state the profile and cite only what it serves, and stderr writes it at
startup ("Profile 'motion': 63 of 210 tools"). Two other profiles (PA10,
PARITE-2026-11): `itw` serves speech,
captions and the session in full, plus what the `monter-une-itw` procedure
cites and what a film needs around it; `reel` serves clip settings, pictures,
sound, generation, motion clips and the session in full, plus what
`monter-un-reel` cites. Each serves a little more than half of the catalogue:
the gain is smaller than for `motion`. Relayed to the window, which serves
the whole catalogue, the stdio server itself answers `initialize`,
`tools/list` and calls outside the profile. An unknown name, or the option
with no value, stops startup (`ValidationFailed`, with the possible names).
`--tools --profile <nom>` prints what the profile lists. A profile chooses
what is shown to the client; it is not a permission.

Over HTTP, the server answers only a local client. Listening on the loopback
rules out other machines, but not a web page open in a browser on this one:
every request is therefore checked before reaching a tool. `Host`, if sent,
must designate `127.0.0.1`, `localhost` or `[::1]`, with or without a port.
`Origin`, if sent, must be a loopback origin (`http://127.0.0.1`,
`http://localhost` or `http://[::1]`, whatever the port): any other value,
`null` included, gets 403. A `POST` must announce `Content-Type:
application/json`, otherwise 415. The server never returns an
`Access-Control-*` header. The JSON body of a refusal says what to correct. A
client that is not a browser (curl, an MCP client) sends no `Origin` and
passes through, provided that it announces `application/json`.

`--token <jeton>` additionally requires `Authorization: Bearer <jeton>` on
every request; without it, or with another, the response is 401 with
`WWW-Authenticate: Bearer`. Without the option, the token is read from
`CUTMACHINE_MCP_TOKEN` (environment, or `~/.config/cutmachine/.env`); the
option wins. The variable is preferable: a process's arguments can be read
with `ps` from any account on the machine, and the binary can only erase the
value after reading it. The token is never displayed or logged; empty or
unusable (blank, control character, non-ASCII), it prevents the server from
starting. Without a token, any process on the machine that can open the port
is still accepted: existing configurations point at the bare URL. `--stdio`
refuses `--token` and does not read the variable.

`initialize` answers the requested protocol version if the server knows it
(`2024-11-05`, `2025-03-26`, `2025-06-18`, `2025-11-25`), and `2025-11-25`
otherwise; `serverInfo` carries the version that `cutmachine --version`
returns.

The server also serves the editing procedures that the binary embeds, the
repository's skills (`monter-une-itw`, `motion-design`, `monter-un-reel`).
`get_procedure` answers with no project open: with no argument the list
(name, description, files and sizes), with `name` the `SKILL.md` and its
files, with `name` and `file` that file; a Markdown file comes with its links
resolved (`links`). An absolute path, or one that contains `..`, is refused.
Each procedure is also an MCP prompt (`prompts/list`, `prompts/get`);
`initialize` declares the `prompts` capability.
`--procedure [<nom> [<fichier>]]` returns the same answers.

`train_caption_breaks` also answers with no project open: it trains the
learned splitting of title cards on delivered subtitles and measures it
against the rule (RM3, see [Export](export.md#captions)). On the command
line: `--train-caption-breaks <corpus>... [--evaluate <corpus>]... [--install]
[--folds <n>] [--epochs <n>] [--step <n>]` returns the same answer, each
relative path read from the current folder.

The server receives JSON-RPC batches whatever the version: an array of
responses, none for a notification; `initialize` is refused in a batch. Over
HTTP, an `MCP-Protocol-Version` that does not name one of the four versions
gets 400; without the header nothing changes. A JSON-RPC response posted by a
client gets 400 (it is ignored on stdio). A body is read by its
`Content-Length` alone: two different values, or anything other than a
number, get 400; a `chunked` body gets 411. A `GET` gets 405 with `Allow:
POST`. `--port` takes an integer from 1 to 65535, or 0 for a free port chosen
by the system and displayed in the banner.

`--mcp-serve --stdio` serves the same protocol on standard input and output,
one JSON-RPC message per line: this is the transport that an MCP client
launches itself. The repository declares one in [`.mcp.json`](../../.mcp.json),
so that a Claude Code session opened here sees the engine's tools
(`mcp__cutmachine__describe`, `mcp__cutmachine__insert_clip`…) with no server
to launch by hand and no `--call-tool` in a shell — provided that
`build/cutmachine` is built; Claude Code asks once to approve this server,
and `claude mcp reset-project-choices` reverts that choice. This transport
takes no `--port`; standard output carries only the protocol, every other
message (the banner, the logs) goes to standard error, and closing standard
input stops the server, which releases the project. One server per session,
one project open at a time: everything the previous paragraph says applies,
except for a project that the application holds open: `open_project` is not
refused, the server then relays each call to the window that holds it (AX1),
and its response says so (`app: true`). The window keeps its project until
CUTMACHINE closes: `create_project` and `open_project` of another project are
refused there with `UnsupportedOperation`, with the path to another project,
and `ingest_media` goes through the window's import, with the response of
`--ingest` (F2.4, see [Built-in agent](agent-integre.md#the-panels-project)).
In this mode, `get_editor_view` reads what the editor is looking at: the
timeline on screen, the playhead (exact, and as a frame of the sequence), the
clips under the playhead on each track with the source frame each plays
there, the selection, the markers the playhead is on or inside (a marker with
`duration_frames` is a span), and the In/Out range the editor has set
(`marked_range`: `in` and `out` as `delete_timeline_range` takes them, the
out exclusive, `valid` when both are set in order). This is what an agent
reads when the editor says "this clip", "here", "this passage", "between the
in and the out". Interface state, read-only; a server with no window refuses
it with `UnsupportedOperation` (AX5, D5).

`capture_editor_window` shows the same window as an image (VQA5): the app
draws it itself, off screen, with the title bar, the panels, Source, Record
and the timeline, so that the capture works with the window covered or behind
another application, without screen recording permission. `region` chooses
the whole window (`window`, the default) or one surface (`timeline`,
`record`, `source`; a hidden surface is refused with `ValidationFailed`),
or the Charte panel (`charter`, refused when the panel is closed),
`scale` one pixel per point (1, the default) or two. The response carries the
image as JPEG and a text: its size, and where each surface is in it, in
pixels from the top left corner. What macOS adds when compositing the screen
is missing: the glass of scroll edges and of the search field, and the
dimming of secondary labels, lighter here than on screen. Record keeps its
previous image for a moment when a clip is waiting for its decoder: the
capture is then refused (`IoError`) rather than returned with a hole. A
server with no window refuses it with `UnsupportedOperation`.

### Editor video output

`get_video_output` lists the connected screens and the state of the clean
feed: `selected_screen_id` keeps the local choice, `active_screen_id` names
the output actually lit, `enabled` says whether it is. Each screen carries
its stable UUID (`id`), its name, its dimensions in pixels and
`editor_screen`. `set_video_output` chooses this UUID with `screen_id`, or an
empty string to stop the output. The editor's screen and an absent screen are
refused with `ValidationFailed`, without losing the previous choice. The
choice is saved in the machine's preferences, with no editing operation. The
presentation is `rec709_sdr`, like Record. Both tools require a project
relayed to the window (`app: true`); a server with no window refuses them
with `UnsupportedOperation`.

### Engine log

A server (`--mcp-serve`, stdio or HTTP) and the application write one line
per event in `~/Library/Logs/CUTMACHINE/cutmachine.log`, and on Linux in
`~/.local/state/cutmachine/cutmachine.log` (PRODUIT-2026-10b PRD35,
HEADLESS-2026-10). The time is in UTC, to the millisecond.

- **At startup**: the surface (`mcp-stdio`, `mcp-http`, `app`), the version,
  the commit (`-dirty` if the tree differed) and the pid.
- **For each tool call**: the tool, `ok` or the name of the error, the
  duration in milliseconds, and the timeline when the call names a
  `timeline_id`.
- **At shutdown**: `stop`, or `stop signal=15` if the process is stopped by
  `SIGTERM`, `SIGINT` or `SIGHUP`.
- **If the process crashes**: `crash signal=11` or `crash terminate`. The
  system's crash report follows as usual, and the pid links it to the line.

Never the arguments or the responses: a tool name or a `timeline_id` that
does not have the shape of an identifier is written `?`. One-off commands
(`--describe`, `--call-tool`, `--export`…) write nothing. What the server
session answers on its own (`get_task`, `cancel_task`, `list_tasks`, replay
of an idempotency key) has no line.

Several processes write the same file. It rolls over to `cutmachine.log.1`
at 5 MB, and only one old file is kept. `CUTMACHINE_LOG_DIR` replaces the
folder (the test suites write into `build/test-logs`), and
`CUTMACHINE_LOG=0` turns everything off.

An agent intervention is undone with a single Undo. `--apply-op` accepts a
batch, `{"type":"Batch","label":"…","operations":[…]}`: at most 256
operations, applied in order and refused as a whole as soon as one is, which
form a single journal entry. A client whose intervention spans several tool
calls brackets it: `begin_intervention` returns a token without writing
anything, `end_intervention` groups into one named entry the steps applied
since then to this timeline — a single step too, which becomes a batch of one
operation under the label (PRD40) —, and refuses, leaving the history as it
is, if the history has been undone below the start of the intervention. The
application's Agent panel does this at each instruction, unless the edit was
modified by hand during the turn.

```sh
./build/cutmachine --call-tool ./Film.cutmachine-project/project.cutmachine.json \
  begin_intervention '{}'
./build/cutmachine --call-tool ./Film.cutmachine-project/project.cutmachine.json \
  end_intervention '{"intervention":"<jeton>","label":"Resserrer la question 2"}'
```

An agent can also **propose** an intervention instead of applying it: this is
a second way of working, next to direct editing, not in its place.
`prepare_edit_plan` executes steps — editing tools of the catalogue with
their arguments — on a copy of the timeline: nothing is saved, journaled or
activated. Aliases (`A1`, `A2`…) are resolved only once, on the timeline as
it is at the start: deleting `A1` then `A2` deletes the two clips seen, never
the former `A3`. The response is measured by the engine: number of
operations, durations before and after, delta (created identifiers
included), report of each step, and `base_doc_hash`, the revision of the
timeline on which the plan was prepared. `apply_edit_plan` applies it as a
single undo step, with the same identifiers as the preview; if the timeline
has changed in the meantime, it refuses with `StaleRevision` without applying
anything. `get_edit_plan` returns the plan serialised by the engine
(`plan_json`, its exact operations and the generated identifiers), without
writing anything: this is what you keep before `commit_run_step`, so that a
server restart does not change the proposed edit (see
[Durable steps](orchestration-montage.md)). `discard_edit_plan` forgets it.
Whoever drives decides: an MCP client can apply its own plans. A server keeps
at most 8 plans per timeline in memory (64 MiB in all), never in the project.
Since each CLI call is a separate session, the CLI keeps the plan in a file
that you name for it, outside the project, and `--call-tool` refuses
`prepare_edit_plan`, `apply_edit_plan` and `discard_edit_plan` (it finds no
plan to return for `get_edit_plan`, `UnknownPlan`):

```sh
./build/cutmachine --prepare-edit-plan ./Film.cutmachine-project/project.cutmachine.json \
  '{"timeline_id":"<timeline-id>","label":"Resserrer le début","steps":[{"tool":"remove_clip","arguments":{"clip_id":"A1"}},{"tool":"remove_clip","arguments":{"clip_id":"A2"}}]}' \
  ./plan.json
./build/cutmachine --apply-edit-plan ./Film.cutmachine-project/project.cutmachine.json ./plan.json
./build/cutmachine --discard-edit-plan ./plan.json
```

Without `--word-by-word`, the SRT starts from the exact limits of the
word-level transcription, then groups them by temporal proximity and French
syntactic unit. The overall splitting aims at **18 characters per second on
average**: this is not a ceiling per title card, so a proposal may
occasionally exceed 18 CPS in order not to separate a determiner from its
noun, a subject from its verb or a preposition from its complement. Sentence
endings and pauses of at least 700 ms remain mandatory boundaries.
`--word-by-word` keeps every raw limit for alignment checking in Resolve.

`--after-request` drives the local After Effects instance with the same
strict contract as the MCP tools and the built-in chat. The safe workflow
first opens a new working copy with `open_project_copy`, then inventories the
footage with `list_footage` — an offline media item is rendered as SMPTE bars
without raising an error — and the IDs with `list_comps` and `describe_comp`
before any mutation. `render_frame` writes a frame as PNG: it is the only way
to look at the result of a modification instead of deducing it from a
property tree. See
[`tools/after-bridge/README.md`](../../tools/after-bridge/README.md).

The MCP tools that modify a timeline also accept the optional `timeline_id`
field. Without this field, they keep the historical behaviour and target the
active timeline. To make the identifier mandatory on this machine — useful
for scripts that drive several edits — enable the local safeguard then
relaunch CUTMACHINE or `--mcp-serve`:

```sh
defaults write com.cutmachine.editor CMRequireExplicitTimeline -bool true  # CUTMACHINE.app
defaults write cutmachine CMRequireExplicitTimeline -bool true  # build/cutmachine
```

The application reads the `com.cutmachine.editor` domain; the
`build/cutmachine` binary (`--mcp-serve`, `--call-tool`, edit plans) reads
the `cutmachine` domain.

For a session: `open_project` or `create_project` with
`require_timeline: true` (the response says `require_timeline`), or
`./build/cutmachine --mcp-serve <projet> --stdio --require-timeline`. This is
the mode of a series, several agents on several timelines of a project
(skill `monter-une-itw`, `serie.md`).

Any tool that edits **or reads** a timeline is then refused without
`timeline_id` (`TimelineRequired`, which names the project's timelines). This
setting stays in `NSUserDefaults` or in the session and is never written into
the project. Reading a source clip is not reading a timeline: `read_frame`
with `media_id` (without `clip_id`) passes without `timeline_id`.
`media_sheet`, `list_source_words`, `list_stable_windows` and the audio
analyses target no timeline; `locate_source_frame` and
`list_clip_tempo_beats` place their source clip on a timeline and name it.

Reads of a timeline — `describe`, `get_timeline_transcript`,
`timeline_stats`, `check_timeline`, `list_shot_quality`, `contact_sheet`,
`cut_sheet`, `list_disfluencies`, `list_speech_onsets`, `locate_source_frame`,
`read_frame` and `list_multicam_groups` — also take an optional
`timeline_id`, and read the named timeline **without activating it**; in
strict mode, they too must name their timeline. On the CLI, `--describe`,
`--timeline-stats` and `--check-timeline` take `--timeline <id>`. The
application's Agent panel reads only the timeline on screen: it refuses to
read another rather than switch to it.

### History of a timeline

`describe` with `only: ["history"]` returns the journal of the timeline read
(`timeline_id`, otherwise the active one), along with `total` and `applied`.
Its entries come in order, each with:

- its rank;
- its type as the journal writes it, or `Batch`;
- its label if it has one;
- the number of operations it applies;
- `undone: true` if it is beyond the undo cursor.

The response carries only the last 200 entries, and the history is never in
the default response. A `DeleteGap` entry is any edit that writes track
content: a closed gap, an insert or an overwrite from the Source monitor, a
range deletion. `clips_added` and `clips_removed` tell them apart (0 and 0 for
a closed gap). The journal keeps neither date nor author. The creation of a
timeline (`add_timeline`, `clone_timeline`, `create_timeline_variants`,
`create_interview_short`) is a step of the project journal: the timeline's
history starts at its first edit. A single-step intervention appears there as
a `Batch` of one operation, with its label. `--describe` has no equivalent:
`--call-tool <projet> describe '{"only":["history"]}'`. An entry that
carries a motive (below) lists its reasons under `motifs`.

### What changed since a state

> PA9, PARITE-2026-11.

An agent keeps its model of the timeline and corrects it with the delta of
each edit. Between two of its turns, the person edits in the window: the
model is then wrong without anything saying so. `list_changes` takes in
`since` the `doc_hash` of the last edit result the agent saw, and returns
**one** delta from that state to now (clips created, modified, removed,
shifted, like the delta of an edit), with the journal entries in between.
Without `since`, it only names the current state, to be passed back the next
time.

- `changed` says whether the two states differ. The delta has no word for
  everything: a renamed marker, a style setting, a locked track leave an
  empty delta under `changed: true`, and the type of the entries says what to
  reread.
- `undone: true`: the requested state is beyond the undo cursor. The timeline
  has gone back, the listed entries were undone.
- `found: false`: the journal no longer leads to that state (undone, then
  other edits; or more than 500 entries back). It is not an error: reread the
  timeline with `describe`.

The journal keeps no author: the entries that follow the agent's last one are
someone else's. Rejecting in one go what an agent did remains `undo` on its
folded intervention (`end_intervention`).

### The motive of an edit

The document keeps what was done; the journal also keeps why (MOTIF-2026-10).
Every tool that writes into a timeline's journal — all edits,
`end_intervention` and `apply_edit_plan`, but neither `undo` nor `redo`,
which move entries — takes a `motif` argument:

```json
{"reason": "duplicate", "note": "garde la 2e prise", "author": "agent",
 "marker_id": "K3"}
```

- `reason`, a closed vocabulary: `hesitation`, `pause`, `boundary_air`,
  `duplicate` (a repeated point), `selection` (choice of what is said),
  `cutaway`, `rhythm`, `brief`, `marker`, `format`, `captions`, `technical`,
  `other` (which requires a note);
- `note`, a line of at most 280 characters;
- `author`: `agent` (the default), `person` or `procedure`;
- `marker_id`, the marker the edit responds to, resolved like any identifier
  (alias, full ID, prefix);
- `params`, an object, for a caller who wants to record its own thresholds.

A `reason` or a `note` is required. `rule` is refused: only a tool's code
names its rule. A tool that writes into no timeline journal refuses `motif`
by name; the catalogue reads this itself, no tool sees it.
`tighten_pauses`, `clean_disfluencies`, `trim_boundary_air`,
`close_junction_air`, `generate_captions` and `conform_sequence` write their
own reason, their `rule` (their name) and their `params` (thresholds read,
defaults included); the caller adds only `author`, `note` and `marker_id`,
and another reason is refused before any cut. A plan step takes its motive
from its `arguments`; `apply_edit_plan` covers with its motive the members
that no step explained, `end_intervention` likewise for the entries it folds,
each keeping its own. `--apply-op <projet> <op> '<motif>'` sets a motive on
an operation written by hand.

Project edits take it too (MOTIF6): `add_timeline`, `clone_timeline`,
`create_timeline_variants`, `create_interview_short`, `rename_timeline`,
`set_timeline_bin`, `remove_timeline`, `set_active_timeline` and
`choose_timeline_variant`. A project journal entry carries at most one motive
(`add_timeline` with `activate` writes two, each under the call's motive);
its `marker_id` is resolved on the active timeline.
`--apply-project-op <projet> <op> '<motif>'` sets a motive on a project
operation written by hand.

`choose_timeline_variant` writes into the project journal the choice between
timelines (variants, clones, versions): `timeline_id`, the timeline kept;
`alternative_timeline_ids`, those among which it was chosen (at least one,
present, each once); `activate` to make it active. It requires `motif`, with
its `author`: `person` when the person chose, `agent` when the choice was
left to the agent. The choice is a trace: it changes nothing in the project,
except the active timeline if asked.

```json
{"timeline_id": "01K…", "alternative_timeline_ids": ["01K…", "01K…"],
 "motif": {"reason": "rhythm", "author": "person",
           "note": "la plus serrée tient la chute"}}
```

`list_motifs` (`timeline_id`, `reason`, `rule`, `journal`, all optional)
reads the journal back as it stands, grouped by reason, rule and parameters:
each group lists its entries (rank in the history, author, note, marker) and
what each member touched (clip, source ranges cut, air trims).
`operations_without_motif` counts what nobody explained. Undone entries are
not included. With `journal: "project"`, it reads the project journal back:
each entry says the timelines it names (created, source of a clone, variants,
kept one and alternatives of a choice), by their name in the project as it
is, `removed: true` for a timeline removed since; it then takes no
`timeline_id`.

The motive is a trace, not a state: it lives in the journal
(`.editlog.json`), never in the document, and changes nothing about what an
operation does. A journal with no motive is written as version 1, byte for
byte identical to what was written before motives existed; a journal that
carries one is written as version 2. A binary older than MOTIF-2026-10
refuses a version 2 journal, as it refuses a document newer than itself: it
still reads the timeline but refuses to edit it (`ParseError`), and its
window refuses to open the project; this binary refuses a version 3 and says
so (`edit log version 3 is newer than this build reads`). The project journal
follows the same policy: version 1 byte for byte without a motive, version 2
with one, version 3 refused by name (`project edit log version 3 is newer
than this build reads`); a binary older than MOTIF6 refuses the project
journal in version 2.

`transcribe_timeline` directly decodes the source ranges of the audio clips,
puts them back on their exact time grid and passes the assembled PCM to
Whisper without an intermediate video export. Its cache depends on the
timeline's identity, the bounds and positions of its audible clips, the
model, the language, the verbatim mode and the decoding profile: an audio
cut therefore cannot
reuse a stale text. `--srt-from-media` uses the same transcription path to
produce an SRT from a deliverable without creating a throwaway project.

All three transcription commands accept `--decoding-profile`. Without it,
French (`fr`) verbatim decodes with `fr-hesitations-v1` on the s075 weights
when `CUTMACHINE_WHISPER_HESITATIONS_MODEL` names them, and everything else
with `default` on the model; `--decoding-profile default` keeps the model
even then, and `--decoding-profile fr-hesitations-v1` requires French,
`--verbatim` and the s075 weights, as that setting or as the model. The
answer says which profile ran (`decoding_profile`), on which weights
(`whisper_model`), and whether the engine chose it
(`decoding_profile_implicit`). The corresponding MCP argument is
`decoding_profile` on `transcribe_media` and `transcribe_timeline`.
See [the profile and cache provenance](transcription.md#french-hesitation-profile).

SRT counts in milliseconds, and a frame at 24 or 29.97 fps almost never
lands exactly on one. Writing an SRT (`--export-srt`, `--srt-from-media`, the
application's export) therefore rounds each bound to the nearest
millisecond: at most 0.5 ms, less than half a frame at any frame rate under
1000 fps, so that the file read back lands on the same frame. This is the
only rounding on the path, and it is declared: `rounded_timestamps` counts
the rounded bounds. A subtitle shorter than a millisecond is refused, and a
refusal never touches the destination file.

`--export-vtt` writes the same cues as WebVTT, with the same rounding and the
same count; `--export-edl`, `--export-edl-markers` and `--import-edl-markers`
write and read back CMX 3600 lists. Both are described in [CMX 3600 EDL and
WebVTT](edl.md), which says what was checked in Resolve. `--export-otio` and
`--export-fcpxml` write the timeline, all its tracks in one file, for another
editor: [OTIO and FCPXML](echange.md), not yet opened in any editor.

When a B1 speech envelope is available, the transcription cache reports
`speech_assessed`, `measured_speech_duration`, `likely_hallucinated`,
`likely_incomplete` and `known_hallucination_phrase`. Words with no measured
speech group at all are flagged as probably invented; a known subtitling or
subscription phrase only reinforces this diagnosis and is never sufficient on
its own. The MCP tools `list_disfluencies`, `remove_words` and
`create_interview_short` refuse these transcriptions, unless explicitly asked
with `force: true`. Conversely, at least ten seconds of measured speech below
the conservative floor of 30 words/minute trigger `likely_incomplete`: the
text must then not be interpreted as proof that the source clip says nothing.

A new transcription does not replace the cache when it looks worse than it
(PRD40): flagged `likely_incomplete` or `likely_hallucinated` while the cache
is not, or, made the same way (verbatim or not), with fewer than half of its
words. The cache is kept and the media item's result says so
(`cache_kept: true`, `cached_words`, `cache_detail`); `words` and `text`
describe what was just done. `overwrite: true` (MCP) or `--overwrite`
(`--transcribe`) installs it anyway. Transcription launched from the
application's Média menu always writes the cache.

A verbatim pass that the speech envelope flags `likely_incomplete` is redone
without the verbatim prompt, on those media items only (ITW9): on English and
Spanish takes, the verbatim pass returned 2 to 6 words for 13 to 22 s of
voice, the ordinary pass the whole text. When the latter decodes more words
and is not `likely_hallucinated`, its words become the transcription, with
the hesitations of the verbatim pass ("euh", "uh", "um"… of two seconds at
most) where it heard nothing; the result is evaluated again against the
envelope. The cache keeps `verbatim: true`, what was asked for, and says
`plain_pass_words`, the words that came from the ordinary pass: they carry no
other hesitation. The response says so under `plain_pass` (`used`,
`verbatim_words`, `plain_words`, then `kept_verbatim_words`, or `detail` when
the ordinary pass was not retained). `list_source_words`,
`list_disfluencies` and `clean_disfluencies` return `plain_pass_words` next
to `verbatim`. Without an envelope (`analyze_speech_onset` first), nothing is
evaluated or redone; the transcription of a timeline and that of the Média
menu do not do it.

Then, when the words made only of dots or ellipsis characters cover at least
2 s of measured speech, the pass that gave the words (verbatim, or ordinary
when no verbatim was asked for or the ordinary pass took its place) is redone
on those media items with that token forbidden (ITW9). Its
words whose middle falls under an ellipsis laid over at least half a second of
speech take the ellipsis's place, except those that would overlap a kept word.
The words of one ellipsis are refused together, and the ellipsis stays, when
they hold a known credit ("Sous-titrage…", "Sous-titres par…"), loop (four
words or more, fewer than half of them distinct) or are too few for the speech
covered (below the 30 words per minute of `likely_incomplete`). What a verbatim
pass leaves under ellipses is then asked of the ordinary pass, the token still
forbidden, with the `default` decoding profile. The result is evaluated again
against the envelope. The cache says `ellipsis_pass_words`, which
`list_source_words` returns; the response says so under `ellipsis_pass`
(`speech_under_ellipses`, `ellipses`, `filled`, `refused`, `words`,
`plain_words` for the words of the ordinary pass, and `detail` when nothing
was put in place). Each media item is decoded on a whisper state of its own: a
temperature fallback samples at random, and on a shared state the draw
depended on the media items decoded before.

`--apply-project-op` accepts either a canonical operation or an array. In the
second case, the operations are applied in order and form a single step of
`--undo-project-op`; a refusal cancels the whole batch. A project operation
does not load the journals of the timelines it does not modify, and the save
preserves their existing bytes.

## Multicam

A multicam group gathers clips already placed on picture tracks, one per
synchronised camera, and designates an active angle. As long as an angle is
active, the group's other angles disappear from the picture over their whole
duration, as if their track were hidden at that spot; what lies beneath
remains visible where the active angle does not cover. Without an active
angle, all the angles composite like ordinary clips. The same rule
(`src/engine/Multicam.h`) serves the monitor, export, `verify_export`,
`timeline_stats`, the contact sheets, `list_shot_quality` and the return to
Resolve; the latter refuses a hidden angle whose sound still plays, because
it does not know how to send that sound without its picture.

Multicam touches only the picture: sound clips, linked or not, keep playing.
Muting a camera's sound is done by muting its track.

The four commands resolve identifiers by full ULID or by unambiguous prefix,
take `--timeline` and return, in addition to the result of `--apply-op`, the
group's identifier, those of its angles, the active angle and the hidden
clips. `--set-multicam-angle` accepts an angle or the clip it shows; `none`
clears the active angle and requires `--group`. Each write command is one
undo step and takes the project's session lock. The corresponding MCP tools
are `add_multicam_group`, `set_multicam_active_angle`,
`remove_multicam_group` and `list_multicam_groups`; in the application, the
Clip menu offers "Activer l'angle multicam du clip" (Activate the clip's
multicam angle) and "Afficher tous les angles multicam" (Show all multicam
angles).

Cutting a group: `--split-multicam-group <projet> <groupe> <position>`
(timeline frames, or `valeur/cadence`), MCP `split_multicam_group`
(`timeline_position`). Each angle that crosses the position is cut there, and
what follows forms a second group, which can show another angle: this is
Resolve's multicam editing, in a scheme where a group has only one active
angle. The group keeps the angles from before the cut; the second takes the
right-hand pieces and the angles entirely after, and shows the angle that was
active, or the one named by `--active` / `active_angle_id` (an angle of the
group). Sound is not cut: it does not follow the angle. A single undo step;
refused when no angle is cut at that position. The response names the second
group, its angles and the one it shows. In the window, the gesture does not
exist yet.
