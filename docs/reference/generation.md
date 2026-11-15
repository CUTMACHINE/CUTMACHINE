# Paid generation (fal.ai)

> Written with the code (GENERATIF-2026-10).
> Proven under Linux against a fake fal.ai. The macOS half
> (`GenerationTransport.mm`) compiles but has not been run; real calls are
> listed under Creative uses.

The engine can ask a paid service to make a medium, with the person's key.
Seven uses: extending an image to another aspect ratio (`expand_image`, model
`fal-ai/bria/expand`), and [six creative uses](#creative-uses). Each
generation is billed to the person's fal.ai account. There is no central
service: the engine talks directly to fal.ai.

The voice-over, by contrast, is generated on the machine, with no service and
no spending (GN6, DEC47): see [the voice-over, locally](#the-voice-over-locally).

## The rule

**Nothing is generated unless the person has explicitly asked for it.** An
agent may propose a generation (`propose_generation`, free); it launches it
(`generate_media`) only after the person's yes, cost included. Without a
request, it proposes and waits. It never sets `confirmed_by_person: true` in
the person's place.

## What generation does not do

Decided in DEC2 of GENERATIF-2026-10, and
written in the description of `generate_media`, of `propose_generation` and in
the `monter-un-reel` and `monter-une-itw` skills:

- **nothing that makes a filmed person say or do what they have neither said
  nor done, without their consent**: no voice, no face, no body. Amended by
  DEC51 (PARITE-2026-11): dubbing, lip-sync
  and an avatar drawn from an image the engine did not generate act on a
  filmed person, and go out only with `consent_of_filmed_person: true`;
- **image extension (`expand_image`) only on a shot with no person in it**;
  the 9:16 version of an interview goes through framing (`set_clip_framing`,
  `track_subject_framing`);
- **generated music only from a model whose licence covers paid advertising**,
  which is verified neither for Stable Audio nor for Lyria (no music model is
  wired in today).

The engine does not check these rules itself: it cannot yet say reliably
whether a shot shows a person. It is an instruction given to the agent, and
the permission prompt of `generate_media` is where the person sees what would
go out.

## The two tools

**`propose_generation`** spends nothing. Arguments: `kind` (`"expand_image"`),
`media_id`, `aspect_ratio` (`"9:16"`), `source_time` (the frame of a source
clip, in media time, exact), and optionally `prompt`, `negative_prompt` (in
English), `seed`, `frames` (length of the clip produced, 125 by default). It
returns:

- `proposal_id`, valid for thirty minutes in this session, once only;
- `request`: what would go to fal.ai (`canvas_size`, `original_image_size`,
  `original_image_location` computed by the engine, the prompt, the seed),
  the source image excepted;
- `announced_cost` (`null`: the engine knows no price) and `cost_note`, which
  points to the model's page;
- `source`, `clip`, `verified` (what has been verified of the API and what has
  not), `ready.key` and `ready.transport`;
- `next`: show the proposal to the person and wait.

The canvas is the smallest one at the requested aspect ratio, with even sides,
that contains the image at its size, centred; refused if it exceeds 5000×5000
pixels or if the image would cover less than 15% of it, the model's two
limits.

**`generate_media`** spends. Arguments: `proposal_id`,
`confirmed_by_person: true`, and `timeline_id` like any editing tool (the
provenance goes into that timeline's history). It sends the source image to
fal.ai's storage (requested expiry: one hour), launches the model, downloads
the image, keeps it in the package (`generated/<proposal_id>.png`) and makes a
clip from it (`generated/<proposal_id>.mp4`, H.264, BT.709), which it adds to
the library through the ordinary ingest, then it sets its provenance. The clip
stood in for a still image, which the library did not accept; it does now
(REEL-2026-09 R4), but generation has not changed and still sets this clip.

Refusals, **with nothing spent**, and the proposal stays valid:
`ConfirmationRequired` (no `confirmed_by_person: true`),
`UnsupportedOperation` (no transport: the version without an interface, under
Linux, generates nothing; or no project open), `GenerationKeyMissing` (no
key). Refusal with nothing spent, the proposal closed: `ProposalUnavailable`
(unknown in this session, expired or already served). After the upload,
`GenerationFailed` or `IoError` say whether fal.ai received the request, and
that it may be billed.

## Creative uses

> PA12, PARITE-2026-11. Requests and responses
> read from each model's public documentation; proven against a fake fal.ai.
> `stock_image`, `avatar` and `dubbing` have been called for real
> (what came back);
> `stock_video` and `lipsync` never.

| `kind` | What it does | What it reads | Model | Announced price |
|---|---|---|---|---|
| `stock_video` | a silent shot made from a text | `prompt`; `duration_seconds` (3 to 15, 5 by default); `aspect_ratio` (`16:9`, `9:16`, `1:1`, the timeline's by default) | `fal-ai/kling-video/v3/pro/text-to-video` | $0.112 per second |
| `stock_image` | an image at the timeline's size | `prompt`; `seed` | `fal-ai/flux-2-pro` | $0.03 for the first megapixel, $0.015 for each after |
| `avatar` | a synthetic presenter who speaks a sound | `media_id` (an image, or a frame of a source clip at `source_time`); `audio_media_id` and its range | `fal-ai/kling-video/ai-avatar/v2/standard` | $0.0562 per second |
| `lipsync` | the lips of a filmed person re-timed to another sound | `media_id` and its range; `audio_media_id` and its range | `fal-ai/sync-lipsync/v2` | $3 per minute |
| `dubbing` | the voices of a film in another language, lips unchanged | `media_id` and its range; `target_language` (two letters) | `fal-ai/elevenlabs/dubbing` | $0.6 per started minute |
| `edit_video` | a shot retouched by a text, its motion kept: a person or a thing replaced, a place or a look changed | `media_id` and its range (3 to 30 s); `prompt` (required); `reference_media_ids` (0 to 4 still images); `resolution` (`720p`, `1080p` by default) | `kling/v4/video-to-video/edit` | $0.094 per second at 720p, $0.141 at 1080p, on the input plus the output |

A range is `source_time` and `duration` (`audio_source_time` and
`audio_duration` for the sound): the whole medium when nothing is said, 300
seconds at most. The engine cuts the excerpt itself (H.264 and AAC, 1920 at
most on the long side; WAV for a sound alone) and sends it to fal.ai's
storage; an excerpt over 90 MB is refused before any spending. The image goes
out as the file encodes it: a log source clip stays log, and what the model
does with it has not been looked at.

**What comes back is not always the requested size**: `stock_image` returned
1072×1920 for 1080×1920, `avatar` a 720×1280 film at 30 frames per second,
with hand gestures; `dubbing` keeps the size, frame rate and duration of the
source clip, returns the sound in mono, and dubs every voice heard in it,
off-screen ones included. The price billed does not come back in the
response.

**`edit_video`** (PA14; called for real). The text says what changes and what
stays; it names the shot `@Video1` and the images `@Image1`, `@Image2`… in
order. Without an image, the text is enough.

- **The model's limits are stated before anything is sent**: a range of 3 to
  30 seconds, no side under 700 pixels (after the long side is reduced to
  1920), at most 4 still images (the model does not publish its own limit;
  four is that of its neighbours).
- **The price is computed in the proposal** (`estimated_cost`): the rate times
  the input duration plus the output duration, in whole seconds. 5 seconds at
  1080p: about $1.41.
- **An HLG or log clip is converted to Rec.709 before it goes out.** The model
  answers "Failed to load video" to a video tagged HLG (measured, twice) and
  paints in Rec.709 whatever it is given. The engine therefore sends the
  clip's Rec.709 image, through the transform that `read_frame` shows under
  colour management, whether or not the project manages colour. The other uses
  always send the image as the file encodes it.
- **What comes back** (one trial): the clip's size (1080×1920), in Rec.709, at
  **24 frames per second** for a 25 fps clip, with a sound track. What the
  text does not name stayed in place in that trial; nothing in the model's
  documentation promises it.
- **The filmed person's consent** is required as soon as the clip or even a
  single reference image was not generated by the engine.

A video excerpt, for any use, is a picture and a sound, nothing else: the
source clip's timecode track and its metadata do not go out.

An argument that belongs to another use is refused by name, rather than
ignored once the money is spent. What the use made enters the library as the
service returns it (`generated/<proposal_id>.mp4` or `.png`), with its
provenance.

**The filmed person's consent.** `lipsync` and `dubbing` always act on a
filmed person; so does an `avatar`, except when its image is a medium the
engine generated. The proposal says so (`needs_consent`), and
`generate_media` then refuses (`ConsentRequired`, with nothing spent, the
proposal staying valid) as long as the call does not carry
`consent_of_filmed_person: true`. The engine cannot verify a consent: like
`confirmed_by_person`, it is an assertion by the caller, which only the person
who signs the film can make. The consent is not written in the document: the
provenance has no field for it.

**No gaze correction.** fal.ai offers no model that does it (searched for;
`fal-ai/eye-correct` answers 404).

## Provenance

A generated medium carries `generation` in the document (`describe` returns
it): `service`, `model`, `kind`, `prompt`, `seed` (the one the service
returned), `created_at` (UTC), `announced_cost`, `request_id`,
`source_media_id`, and for a voice made by the engine `reference_text`, the
words it says. It is set by `SetMediaGenerationOperation`, reversible like any
operation.

Under DEC36 of GENERATIF-2026-10:

- **a provenance is only erased by Annuler (Undo)**: an operation that would
  empty it or replace it with another is refused (`InvalidOperation`, "only
  undo removes it"), whichever surface it arrives from, `--apply-op`
  included; setting the same provenance again changes nothing and goes
  through; the inverse of an ingest (`RemoveIngestedMedia`, DEC32) applied as
  an edit, which would remove the medium and its provenance, is refused in the
  same way;
- **each clip of a generated medium carries the `GÉNÉRÉ` (generated) badge**
  in the timeline, after the agent's mark and before the speed;
- **`check_timeline` lists these clips under `generated`**, always present,
  empty when nothing is generated: the agent's report draws one line per clip
  from it. No mention is imposed at delivery.

There is no confirmation code in the window: the permission prompt of
`generate_media` is the lock. A per-project spending cap will come as a local
preference, once the prices are recorded (GN5). A document that carries a
provenance declares version 20: an older binary refuses it instead of losing
the mark. The image and the clip are in the package: the project reopens and
exports without fal.ai.

## The voice-over, locally

> GN6b of GENERATIF-2026-10, written with
> the code. Proven against a fake script, then with the bench's real models
> on a throwaway project. The installation under `build/voice` is GN6c.

Qwen3-TTS (Apache 2.0) runs on the machine, launched by the engine like MFA
(`scripts/voice_over.py`, see `--check-setup`, item `voice_over`). Nothing
goes to a server, nothing is paid.

**`design_voice`** makes a voice. Arguments: `description` (the voice, in
English: gender, age, timbre, pace, style), `language` (ISO 639-1 code, `fr`
by default, one of the model's ten languages), optional `seed`. The
VoiceDesign model says a reference sentence fixed by the engine in the chosen
language; the WAV goes to `Sound/voix-<id>.wav` in the package, enters the
library and receives its provenance: `service: "local"`,
`kind: "voice_design"`, `model` the repository and revision of the weights,
`prompt` the description, `reference_text` the sentence spoken. Two calls with
the same description give two different voices: keep the one the person
chooses.

**`generate_voice_over`** has this voice say a text. Arguments:
`voice_media_id`, `text`, `language` (the text's, `fr` by default; it may
differ from the voice's), `seed`. The Base model clones the voice from its WAV
and its words, which keeps it the same from one take to the next; the WAV
goes to `Sound/voix-off-<id>.wav`, marked `kind: "voice_over"`,
`source_media_id` the voice, `prompt` the text. The text goes out as is:
numbers, dates and times are written as they should be said when it matters.
The clip is then placed with `insert_clip`.

**The engine only clones its own voices** (DEC49): `generate_voice_over`
refuses (`NotAnEngineVoice`) any medium that is not a voice made by
`design_voice` — a recording, a source clip, an interview, a voice-over. The
refusal comes before the model runs. A provenance is only written by a
generation and only erased by Annuler (DEC36): it cannot be fabricated through
the tools.

**No proposal and no permission prompt** (DEC48): nothing is spent, and the
two tools are not in the list of those that go to the person. But a voice is
only generated if the person has asked for it; the descriptions say so. Each
call blocks while the model runs (about a minute for a voice, twice the
duration of the text for a voice-over): a background task can neither ingest
nor mark a medium, so neither runs in the background. The computation does
not show in the window's task bar and cannot be cancelled there, even when the
call goes through the app.

A document that carries a voice and its words declares version 25: an older
binary refuses it instead of losing the words, without which the voice can no
longer be cloned. The WAVs are in the package: the project reopens and exports
without the model. An edit plan (`prepare_edit_plan`) cannot take
`design_voice`, `generate_voice_over` or `generate_media` as a step: they
ingest what they make, which the plan's copy cannot do.

## The key

Read from the macOS keychain, then from `FAL_KEY`. Never written to a tracked
file, a log, a trace or a response; a fal.ai error that contained it comes out
masked. To store it once:

```sh
security add-generic-password -s com.cutmachine.generation -a fal-api-key -w
```

The key only goes to `queue.fal.run` and `rest.fal.ai`, never to the image's
upload address or to the result's.

## What stops an agent from spending on its own

The engine does not distinguish the person's yes from the agent's: an agent
that writes `confirmed_by_person: true` without having asked has lied. What
stops it is the client that shows the call to the person:

- **Claude Code, through MCP**: the permission for the tool
  `mcp__cutmachine__generate_media`, on the user's side, is what counts. It
  must be asked for on every call: do not put this tool in an `allow` rule,
  and no `bypassPermissions` for a session that can generate. The tool's MCP
  annotations (`External`) say it is not read-only and touches an outside
  service.
- **The Agent panel, through a coding agent (ACP)**: `generate_media` is no
  longer approved by default like the engine's other tools; it goes to the
  person, without the « toujours » (always) option. With nobody to answer, it
  is refused. But an adapter set to allow everything asks the panel for
  nothing.
- **The panel's built-in loop** (Anthropic or Ollama protocol) shows no call
  before launching it: it refuses `generate_media`
  (`ConfirmationRequired`).

The CLI (`--call-tool`) has no session: a proposal does not survive the call
there, and `generate_media` refuses any proposal there.
