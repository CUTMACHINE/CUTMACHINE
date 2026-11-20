# Built-in agent and user key

The **Agent** panel contacts directly either Anthropic's Messages API, or
Ollama's native local API, or a coding agent launched by its command (**Agent
de code (ACP)** (coding agent (ACP)): Claude Code, Codex…, which connects with
its own account). The **Configurer le moteur IA…** (Configure the AI engine…)
button, placed below the conversation, lets you choose the provider, the
model, the base URL, the API key and the ACP command. The same dialog is
available in **CUTMACHINE → Moteur IA…** (AI engine…).

The panel is a window of its own that floats above the editor (ONGLETS-2026-10,
ON3), rather than a tab of the right-hand dock taking the Inspector's place,
on every tab. It is shown and hidden from any tab, by the **Agent** button at
the bottom right of the window, by **Affichage → Agent** (View → Agent) or by
`⌥⌘A`. The first time, it opens at the top right of the editor, under its
title bar; afterwards where it was left, as long as a screen still carries it:
a disconnected screen brings it back near the editor (`src/ui/AgentWindow.h`).
Its position, and whether it was shown, are local preferences. Shown by the
button or the key, it takes the keyboard in its field; hidden, it gives it back
to the editor. In its field, `⌘C`, `⌘X`, `⌘V`, `⌘Z` and `⌘A` act on the text,
never on the edit. Hidden, it still receives its responses and the relayed
agent's log: a dot on the button ("Agent •") signals it until the panel is
shown. It hides with the application when another one comes to the front, and
full-screen video covers it. The **Moteur IA** dialog remains a sheet on the
editor window.

The dialog enables only the fields of the chosen protocol: model, base URL and
key for Anthropic and for Ollama, the command alone for the coding agent; the
others are greyed out, and their prompts follow the menu. It opens on what is
saved; a field left empty saves nothing and takes the value suggested in grey.
To use the Qwen Coder model already installed on the machine, choose **Ollama
local**: model `qwen2.5-coder:7b` and URL `http://localhost:11434` by default.
Local Ollama needs no key; a key that is entered goes out as
`Authorization: Bearer` (an Ollama behind a proxy, or ollama.com). Tool calls
remain available: the model can therefore observe and modify the edit through
the same reversible operations as the Anthropic engine.

The Ollama transport uses `/api/chat` and requests a 32K-token context. Before
sending `describe` back to the local model, the app keeps the timeline, the
sources used, the media library aliases, the bins and the markers, but removes
redundant media metadata. For older models that write a call as the JSON
`{name, arguments}` instead of a real `tool_calls`, this format is executed
only if the object is strict and designates a tool actually published by
CUTMACHINE.

To edit an interview, `get_timeline_transcript` returns selectable spans and
`create_interview_short` **accepts only their `span_id`s** (or the `block_id`s
of `list_speech_blocks`), with the `transcript_revision` read at the same time.
The model copies no time: the engine resolves each identifier to an exact
position, which makes a cut in the middle of a word inexpressible instead of
merely discouraged. If a word straddles a cut, it appears only once, in the
clip that plays its start (or, if that start was cut, in the one that plays the
rest), and its span carries `straddles_cut: true` to signal that its rendering
may be approximate. Attribution to the largest overlap would move a word
straddling a cut into the next clip, where it is not spoken first.
When an idea goes beyond one span — they are cut for the readability of a
subtitle, about 42 characters, which is not an editing unit — `span_id` and
`end_span_id` designate a contiguous interval that the engine merges into a
single range. An interval that spans a silence or changes source is refused,
never silently stitched together.

For the usual A/V shortenings, the MCP tool `shorten_linked_clip` receives only
a clip, an edge and a quantity in frames or seconds. CUTMACHINE itself resolves
all the linked members, the sign of the trim and the exact `RationalTime`, then
applies a single atomic operation. With `preview=true`, the same operation is
validated on a copy through `EditLog` and returned without modifying the
project.

The model and the URL are local `NSUserDefaults` preferences. The key is kept
separately in the macOS Keychain: it is written neither in the project
document, nor in its journal, nor in the preferences. With no saved model, the
panel talks to `claude-opus-5-5` at `https://api.anthropic.com`, with at most
16,000 tokens per call (thinking included; each call of a turn that chains
tools has its own) and a 600 s wait per response. `CUTMACHINE_ANTHROPIC_MODEL`,
failing that `CUTMACHINE_MODEL`, replaces this model, `CUTMACHINE_ANTHROPIC_URL`
the URL and `ANTHROPIC_API_KEY` the key; a model or URL saved in the dialog
takes precedence. An earlier version of the dialog saved the model in use at
each validation: whoever validated it then keeps that model (`defaults read
com.cutmachine.editor AiEngine.Model.v1`) until they empty the Modèle (Model)
field.

The **Agent de code (ACP)** protocol launches by default
`npx -y @agentclientprotocol/claude-agent-acp@0.85.1`: a pinned version,
otherwise `npx` would run the package's latest release at every launch. A
command saved in the dialog is never rewritten; the procedure for raising the
version is in `src/agent/AiEngineForm.h`.

A coding agent asks for permission before each tool call. The panel grants it
itself for the engine's tools, except for those it shows to the person, once
each and without the "always" option: `generate_media`, which spends on their
account (GENERATIF-2026-10 GN2). The `after_*` calls go through without
asking.
With nobody to answer, a shown call is refused. An adapter set to allow
everything asks the panel for nothing, which then has nothing to show.

## The panel's project

The Agent panel works on its window's project, and on it alone, as long as the
window is open; so does an agent relayed to the window, whether it is the
panel's coding agent or a Claude Code session open alongside
(`--mcp-serve --stdio`, see [Commandes headless](commandes-headless.md)). The
built-in loop and the window's access point go through the same adapter
(`MainThreadBackend.h`) and give the same responses.

An edit, an undo, a redo or the closing of an intervention (`end_intervention`)
requested by the panel or by a relayed agent are recorded in the package
before the window takes them, like a mouse gesture. If storage refuses the
write (read-only package, full disk), the call is refused with `IoError`, the
name that `--apply-op` and the MCP server give it, with the system's reason;
the window, the package and the journal stay what they were, and the line
under the timeline says "Modification de l'agent refusée, rien n'a changé"
(Agent edit refused, nothing has changed). The call never answers `ok` with
the delta of an edit that the window has just undone.

`ingest_media` imports source clips like **Fichier → Importer des rushes…**
(File → Import rushes…): the files are read in the background ("Import N
files" in the media task bar, which cancels it), then added by the window's
import, at the root of the media library, with their sound level, their
thumbnail and their waveform; the line under the browser says what came in.
The path is absolute; `recursive` descends into subfolders. The response is
that of `--ingest` (`added`, `skipped`, `ignored`, `errors`), and an import
that could read nothing is refused likewise (`IoError`). Like the menu's
import, it is a step of the timeline's journal (DEC32): `Cmd+Z` takes it back;
`bin_id` files the source clips in a bin.

The background tasks of an agent relayed to the window — an export or an
analysis requested with `async: true` — run on the editor's task manager
(DEC10, AX-2026-09 AX7): the task bar shows them ("Agent · export_timeline",
for example) and cancels them, like the import; `get_task` and `cancel_task`
read and cancel them in the same place. They do not run on a manager private
to the access point, which nothing shows. An export requested without `async`
still does not go through the task bar.

`create_project`, and `open_project` of another project, are refused
(`UnsupportedOperation`): the refusal names the window's project and says how
to change it: the person chooses **Nouveau projet…** (New project…) or **Ouvrir
un projet…** (Open a project…) in the window's **Fichier** (File) menu, which
reopens on the other project (DEC39), rather than quitting CUTMACHINE and going
through the welcome window. `open_project` of the window's project answers,
with `app: true`.

`import_resolve_timeline` reads a Resolve timeline back into the window's
timeline (the one named by `timeline_id`, otherwise the one on screen), with
the response of `--import-resolve-timeline`, the grade read from the Resolve
database included, and written onto its clips when it comes through whole
([Resolve](resolve.md)). The manifest and the copy of the database are read in
the background; the plan and the operations go through the window's journal,
recorded once: the import is undone with `Cmd+Z`. A refusal changes nothing in
the window (PHASES-2026-08 F2.4; not yet seen on screen).
