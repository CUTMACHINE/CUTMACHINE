# CUTMACHINE reference

The engine reference, one file per section. What the project is, what you
need to build it and a first walk-through on the command line are in the
[`README.md`](../../README.md) at the repository root.

- [Launching](lancement.md): opening the application, the development loop,
  [which FFmpeg](lancement.md#which-ffmpeg) the engine launches, storage and
  collecting a project.
- [Time decisions](decisions-de-temps.md): what exact time imposes on
  scrubbing, on the playhead and on track compositing.
- [Graphical timeline](timeline-graphique.md), [Audio](audio.md) and
  [Bins](chutiers.md): the window.
- [Editing API](api-edition.md): the editing operations and tools, the
  settings of a clip, and the [catalogue of tools by family](api-edition.md#the-catalogue-by-family).
- [Headless commands](commandes-headless.md): the command line, the MCP
  server, interventions and edit plans. `cutmachine --help` points to it.
- [SQL archive of projects](archive-projets.md): keeping the full JSON on
  save, importing a collection and querying it with SQL/JSON, from the CLI
  or over MCP.
- [Durable steps and agent measurements](orchestration-montage.md): step
  receipts with no double edit, linked captures, captions inside a plan, and
  the report of the Agent panel's ACP traces.
- [Import from DaVinci Resolve](resolve.md) and
  [Sequence format](format-de-sequence.md).
- [Verbatim transcription and cleaning up
  hesitations](transcription.md): the
  Whisper model, words, silences.
- [Shot quality control](controle-qualite.md),
  [Looking at a shot](regarder.md#looking-at-a-shot),
  [Looking at the edit as contact sheets](regarder.md#looking-at-the-edit-as-sheets),
  [Looking at a cut as it is delivered](regarder.md#looking-at-a-junction-as-it-is-delivered),
  [Viewing the cuts](regarder.md#reviewing-the-junctions),
  [Describing and searching shots](regarder.md#describing-shots-once-searching-them-afterwards) and
  [Measuring the grade](mesurer-etalonnage.md): what the engine measures and
  shows.
- [CMX 3600 EDL and WebVTT](edl.md): lists of a timeline and its
  markers, reading markers back, WebVTT subtitles.
- [OTIO and FCPXML](echange.md): a timeline written for another
  editor, all its tracks in one file.
- [Final video export](export.md): export, its verification, captions,
  sound measurement.
- [Sequence](sequence.md): the document, colour management, project
  operations.
- [Built-in agent and user key](agent-integre.md) and
  [Conversational sidecar](sidecar.md): driving by a model.
- [Paid generation](generation.md): proposing then generating with fal.ai,
  on the person's own key and only on their request; provenance.
- [Reference images in tests](tests-golden.md): comparing a CPU render
  with a PNG image, with its tolerance, and rewriting the references.
- [Licence](../../LISEZMOI.md#licence).
