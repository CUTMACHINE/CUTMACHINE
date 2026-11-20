# Architecture

`src/` holds about 550 files in seven modules, one directory each, read off
the include graph rather than the file names: a file belongs where what it
includes, and what includes it, put it. `tests/architecture_tests.py` holds
the rule below. A header is included by its bare name (`#include
"Document.h"`) from any module: every module directory is on the include
path, so two files may not share a name.

| Module | What it holds | Files |
|---|---|---|
| `engine` | The document and its algebra: exact time, identifiers, the `Document`, its operations and journal, the project, its formats and storage, and the document-side data of clips and motion compositions. | 131 |
| `media` | What reads, measures and writes media, mostly through FFmpeg: decoding helpers, audio DSP, export and its verification, transcription and speech, picture and colour analysis, captions, interchange formats (EDL, Resolve, OTIO, FCPXML). | 156 |
| `motion` | Motion graphics above export: the template library, the After Effects bridge, placement, safe areas, humanizing. | 25 |
| `ui` | The interface's policy in portable C++, tested without AppKit: the theme, the panels' layout, the timeline's drawn and clickable geometry, the inspector's models, playback's read-ahead plan. | 59 |
| `agent` | The surfaces that drive the engine: the command line, the MCP server and its tools, the ACP client, the chat session, the optional production connector. | 65 |
| `platform` | The Apple side of the seams the model declares: Core Text and Core Graphics renderers, Vision, `NSURLSession` and the Keychain, JavaScriptCore. | 16 |
| `app` | The macOS window: AppKit, Metal, playback and its decoders, `AppDelegate` and its categories, the window test's seam. | 98 |

## The rule

A file includes only headers of its own module or of a module before it in
this order:

    engine < media < motion < ui < agent < platform < app

No file of `engine`, `media`, `motion`, `ui` or `agent` includes an Apple
framework. `CommonCrypto` is the one exception: the build without a window
answers it with OpenSSL (`headless/compat/`).

Six includes go against the order today. The guard names them, and the list
can only shrink:

| From | Includes | Why it is there |
|---|---|---|
| `engine` `ProjectStorage.cc` | the optional connector's header (`agent`) | a save is reported to it, when it is compiled in |
| `media` `SetupCheck.cc` | the same header | the setup report's element for it, likewise |
| `media` `Edl.cc` | `agent` `Cli.h` | the marker commands apply an operation as `--apply-op` does |
| `media` `Edl.cc` | `ui` `TimelineView.h` | the window's marker colours, so a list read back keeps them |
| `media` `TimelineCheck.h` | `motion` `MotionTemplates.h` | the delivery check reports the motion kit the clips use (`MotionKitInfo`) |
| `ui` `AgentJournal.h` | `agent` `McpEndpoint.h` | the window's journal records the endpoint's events (`McpEndpoint::Event`) |

## What this does not change yet

The model is still one library (`cutmachine_model`), next to
`cutmachine_motion_render` (the `platform` renderers) and
`cutmachine_app_objects` (the window); each lists its sources by path. One
library per module waits on the six includes above: each is a link
dependency against the order, and together they make cycles between
libraries. Inside a module, cycles are many and harmless (`Document.cc`
validates clip features whose headers include `Document.h`).

Two placements follow what compiles a file rather than what it is about:
`DecodeWorker`, `FrameCache`, `AudioDecode` and `PerformanceMetrics` are
portable C++, but `DecodeWorker.h` includes `MediaSource.h` (Objective-C++),
so playback stays in `app`; `ReadAhead.cc` lives with its header in `ui`
while the app's library compiles it.
