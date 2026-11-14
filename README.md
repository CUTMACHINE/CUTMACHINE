# CUTMACHINE

[![CTest](https://github.com/CUTMACHINE/CUTMACHINE/actions/workflows/ctest.yml/badge.svg)](https://github.com/CUTMACHINE/CUTMACHINE/actions/workflows/ctest.yml)

An addressable video editing engine, built to be driven by an AI agent.

CUTMACHINE is a document model, an algebra of operations that transforms it,
and several surfaces that expose both. The macOS window is one surface, the
command line another, and the MCP server through which an agent edits a
third. A mouse gesture and a tool call produce the same operation, go through
the same journal and pass the same validations. A project is a set of
readable JSON files, every change is a named, reversible operation, and
positions and durations are exact rationals, never floats.

> The detailed documentation is in French: [`LISEZMOI.md`](LISEZMOI.md)
> (building, testing, a first walk through the command line) and the
> reference under [`docs/reference/`](docs/reference/README.md). Code
> comments are in English.

## What it does

- **Editing**: insert and overwrite, ripple, roll, slip and slide trims,
  linked picture and sound, multicam groups, speed and speed ramps,
  transitions, markers, bins, several timelines per project, and timeline
  variants (formats, versions) made in one step.
- **Speech**: transcription with whisper.cpp, word boundaries refined by
  forced alignment (Montreal Forced Aligner, optional), hesitations and
  restarts found and removed, pauses tightened, interviews cut down from a
  selection of sentences, speakers told apart.
- **Pictures**: shot quality and stable windows measured, frames, contact
  sheets and cut sheets to look at, subject tracking for reframing,
  stabilisation, masks.
- **Colour**: managed colour through OpenColorIO, input transforms per rush,
  CDL, grades measured, proposed and matched, HLG delivery, and DaVinci
  Resolve grades read from a project database.
- **Captions**: word-timed captions in caption styles, rendered by the engine
  (Core Text), with legibility checks and word-by-word animation.
- **Motion design**: compositions and templates rendered by the engine, laid
  out with Yoga, placed on the timeline, exported to and imported from After
  Effects through a local bridge.
- **Sound**: loudness measured and targeted (EBU R 128), ducking, beats and
  music structure, sound stems.
- **Interchange**: EDL (CMX 3600), WebVTT and SRT, FCPXML, OpenTimelineIO,
  Resolve XML and a Resolve plugin, and timelines imported from Resolve.
- **Export**: FFmpeg, with VideoToolbox on macOS, verified after writing.
- **Generation, on request only**: paid generation through fal.ai on the
  user's own key, after an explicit yes, and a local voice-over model.

Every capability exists in the engine first, then in the command line, then
in the MCP server, then in the window.

## Status

This is a personal project, developed by its author for their own editing
work and published as it is. It has no binary release.

- The application runs on macOS only. It is developed on Apple Silicon with
  a recent macOS; other setups are untested.
- The engine, its command line and its MCP server also build on Linux, without
  a window: see [`headless/README.md`](headless/README.md). Captions, motion
  design and hardware encoding need Apple frameworks and are refused there,
  by name.
- About two hundred test suites cover the engine. The continuous integration
  builds and tests the Linux build; the macOS suite, window included, is run
  locally.
- The project format is versioned and older projects are migrated when they
  are read, but nothing here is a stable API yet.
- It was developed in a private repository; the public history starts with
  its first publication, so earlier changes are not visible here.

## Before you build or run it

Building and running this code executes it on your machine. What it does,
so you can check it before you trust it:

- **What the build downloads.** Seven source archives, each at a pinned
  commit and checked against its SHA-256 before use (`FetchContent_Declare`
  in [`CMakeLists.txt`](CMakeLists.txt)): whisper.cpp, OpenColorIO, Yoga,
  SQLite and zstd from GitHub, Strudel from Codeberg, Fraction.js from
  GitHub. One exception: while it builds, OpenColorIO clones six small
  libraries of its own with git, at tags, without a checksum. Every
  third-party component is listed in
  [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md). The build downloads no
  model and no binary.
- **What the tests run.** The engine's own test binaries, plus Python and
  Node scripts from [`tests/`](tests/) and [`scripts/`](scripts/), on
  throwaway files. They need no network and no account.
- **What talks to the network at run time.** Nothing by default. Paid
  generation goes to fal.ai, only with a key you provide and after an
  explicit yes for each request. The editor's agent panel talks to the model
  you configure: the Anthropic API with your key, a local model, or a coding
  agent you choose to run. Whisper, the aligner and the local voice-over run
  on your machine, with models you install yourself.
- **What opening this folder in Claude Code does.** [`.mcp.json`](.mcp.json)
  declares one local MCP server, the binary you built
  (`build/cutmachine --mcp-serve --stdio`); nothing remote.
  [`CLAUDE.md`](CLAUDE.md) and the procedures under
  [`.claude/skills/`](.claude/skills/) are plain Markdown instructions for
  the agent: read them like you would read any prompt you hand to a model.
- **Safer ways to try it.** The Linux build without a window runs in a
  container ([`headless/README.md`](headless/README.md)); on macOS, a
  separate user account keeps it away from your files and keychain.
- **Reporting a security issue.** Please use GitHub's private vulnerability
  reporting (the repository's Security tab, then "Report a vulnerability")
  rather than a public issue.

## Building

Requirements on macOS: Xcode's compiler and SDK, CMake 3.24 or later,
pkg-config, FFmpeg (its libraries and the `ffmpeg` executable) and Python 3
for the tests. `brew bundle --file=Brewfile` installs them.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
ctest --test-dir build --output-on-failure
```

The first configuration needs the network: CMake downloads whisper.cpp,
OpenColorIO, Yoga, SQLite and zstd as archives of pinned commits and checks
their SHA-256 before using them. The first build compiles them too, which
takes a while.

Once built, `./build/cutmachine --check-setup` lists what this machine lacks,
item by item (FFmpeg, a Whisper model, the aligner, the agent's key, the
local voice-over), with what each absence prevents and how to fix it.
`./build/cutmachine --help` lists every command.

## A first project from the command line

Each command answers a JSON object on standard output.

```sh
DIR=/tmp/cutmachine-try && mkdir -p "$DIR"
ffmpeg -f lavfi -i testsrc=duration=4:size=1280x720:rate=25 \
  -pix_fmt yuv420p "$DIR/rush.mp4"
P="$DIR/Try.cutmachine-project"

./build/cutmachine --create-project "$P" "Try" --format 1280x720@25
./build/cutmachine --ingest "$P" "$DIR/rush.mp4"
./build/cutmachine --describe "$P" | python3 -m json.tool

# Two seconds of the rush, picture and linked sound, at the start of the
# timeline. M1 is the rush, T1 and T2 the picture and sound tracks.
./build/cutmachine --call-tool "$P" edit_source_range \
  '{"media_id":"M1","track_id":"T1","audio_track_ids":["T2"],"mode":"insert","timeline_in":{"value":0,"rate":25}}'

./build/cutmachine --export "$P" "$DIR/try.mp4"
```

[`LISEZMOI.md`](LISEZMOI.md) walks through the same steps with the raw
operations (`--apply-op`) and explains each answer.

## Driving it with an agent

`./build/cutmachine --mcp-serve --stdio` serves the engine's tools over MCP.
The repository declares it in [`.mcp.json`](.mcp.json): a Claude Code session
opened here sees the `mcp__cutmachine__*` tools as soon as the binary is
built. When the application has the project open, the server relays every
call to it, so the agent and the person edit the same document.

- `--profile motion`, `itw` or `reel` serves only part of the catalogue, for
  a client that loads every tool it is given; `--tools --profile <name>`
  prints what a profile lists.
- The editing procedures under [`.claude/skills/`](.claude/skills/) (an
  interview, a reel, motion design) are compiled into the binary and served
  by the `get_procedure` tool, so any MCP client gets them.
- Analyses measure, they never judge: what to cut or keep stays the agent's
  decision, or the person's.

## Documentation

All in French:

- [`LISEZMOI.md`](LISEZMOI.md): building, testing, the first project in
  detail.
- [`docs/reference/`](docs/reference/README.md): the reference, one file per
  section (operations and tools, command line and MCP server, export,
  colour, captions, interchange, Resolve).
- [`docs/MOTION_FORMAT.md`](docs/MOTION_FORMAT.md): the motion design format.
- [`headless/README.md`](headless/README.md): the build without a window.

## License

CUTMACHINE is distributed under the GNU Affero General Public License,
version 3 or any later version: see [`LICENSE`](LICENSE). The third-party
code and resources it builds with, their versions and licenses, are listed in
[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).
