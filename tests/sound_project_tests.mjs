// SN2 -- docs/roadmap/SON-2026-10.md. The edit a ditty reads, taken from projects the engine
// writes, against the EDL the engine exports from the same timeline: at 25 and at 23.976 frames
// per second, with a centred dissolve of an odd number of frames, one starting at its cut, a gap,
// a fade from and to black and a hidden video track. Both paths must give the same cuts, and a
// click rendered on each cut must land on its exact sample, at 48 and at 44.1 kHz -- from the
// project and from the EDL alike, in the same bytes. SN4: the stem, ingested, is laid back by
// place_sound_stem on the timeline's first frame, over the edit's frames, in one undo. SN7: the
// engine's render_sound, through cutmachine-sound-host, writes the same file.
//
// CUTMACHINE_SOUND_FIXTURES=<folder> keeps the projects and their EDLs there: that is how
// tools/cutmachine-sound/test/fixtures/ was made, for the suite that runs without the engine.
import assert from "node:assert/strict";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { createHash } from "node:crypto";
import { execFileSync, spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const sound = path.join(root, "tools/cutmachine-sound");
const { editFromEdl } = await import(path.join(sound, "edl.mjs"));
const { editFromProject, readProject } = await import(path.join(sound, "project.mjs"));
const { frameToSample } = await import(path.join(sound, "time.mjs"));

const [engine, ffmpeg] = process.argv.slice(2);
if (!engine || !fs.existsSync(engine) || !ffmpeg || !fs.existsSync(ffmpeg)) {
  console.log("SKIP: usage: node sound_project_tests.mjs <cutmachine> <ffmpeg>");
  process.exit(77);
}
// Never into the person's project archive nor their engine log.
const env = { ...process.env, CUTMACHINE_LOG: "0", CUTMACHINE_PROJECT_ARCHIVE: "off" };
const work = fs.mkdtempSync(path.join(os.tmpdir(), "cutmachine-sound-test-"));
const keep = process.env.CUTMACHINE_SOUND_FIXTURES;

const run = (args) => {
  const out = execFileSync(engine, args, { env, encoding: "utf8" });
  const answer = JSON.parse(out);
  assert.equal(answer.ok, true, `${args.slice(0, 2).join(" ")}: ${out}`);
  return answer;
};
const tool = (project, name, args) => run(["--call-tool", project, name, JSON.stringify(args)]).result;

const CLICK = `
const clic = synth.def(class { constructor() { this.n = 0; } process() { return this.n++ === 0 ? 1 : 0; } });
loop(() => { for (const c of edit.cuts) at(c, () => clic.play(0, { attack: 0, release: 0.001 })); }, { name: 'clics' });
`;
const ditty = path.join(work, "clic.js");
fs.writeFileSync(ditty, CLICK);

function render(montage, sampleRate, out) {
  const r = spawnSync(process.execPath, [path.join(sound, "cli.mjs"), "render", ditty, ...montage,
    "--sr", String(sampleRate), "--out", out], { encoding: "utf8" });
  assert.equal(r.status, 0, r.stderr);
  return JSON.parse(r.stdout);
}

// The samples of a 24-bit stereo WAV's left channel above half scale.
function clicks(file) {
  const b = fs.readFileSync(file);
  const at = b.indexOf("data");
  const size = b.readUInt32LE(at + 4);
  const hits = [];
  for (let i = 0; i < size / 6; i++) if (Math.abs(b.readIntLE(at + 8 + i * 6, 3)) > 4194304) hits.push(i);
  return hits;
}

const RATES = [
  { name: "25", num: 25, den: 1, edlFps: "25" },
  { name: "23976", num: 24000, den: 1001, edlFps: "23.976" },
];

try {
  for (const rate of RATES) {
    const t = (frames) => ({ value: frames * rate.den, rate: rate.num });
    const rush = path.join(work, `rush${rate.name}.mov`);
    execFileSync(ffmpeg, ["-v", "error", "-f", "lavfi", "-i",
      `testsrc2=size=160x90:rate=${rate.num}/${rate.den}:duration=12`, "-c:v", "libx264",
      "-pix_fmt", "yuv420p", rush]);
    const project = path.join(work, `projet-${rate.name}`);
    const created = run(["--create-project", project, `Son ${rate.name}`,
      "--format", `320x180@${rate.num}/${rate.den}`]);
    const v1 = created.tracks.find((k) => k.kind === "video").id;
    run(["--ingest", project, rush]);
    const media = run(["--describe", project]).library[0].id;

    // Five shots and a gap of five frames before the fourth.
    const clipIds = [];
    for (const [sourceIn, at, length] of [[10, 0, 50], [70, 50, 13], [90, 63, 7], [100, 75, 61], [180, 136, 40]]) {
      const answer = run(["--apply-op", project, JSON.stringify({
        type: "InsertClip", track_id: v1, source_id: media, source_in: t(sourceIn), duration: t(length),
        timeline_in: t(at), clip_id: "", exact_timeline: [] })]);
      clipIds.push(answer.delta.clips.find((c) => c.track_id === v1 && !clipIds.includes(c.clip_id)).clip_id);
    }
    const [a, b, c, d, e] = clipIds;
    tool(project, "add_transition", { track_id: v1, right_clip_id: a, type: "dip_to_color", duration: t(8), alignment: "StartAtCut" });
    tool(project, "add_transition", { track_id: v1, left_clip_id: a, right_clip_id: b, duration: t(9) });
    tool(project, "add_transition", { track_id: v1, left_clip_id: b, right_clip_id: c, duration: t(6), alignment: "StartAtCut" });
    tool(project, "add_transition", { track_id: v1, left_clip_id: e, type: "dip_to_color", duration: t(10), alignment: "EndAtCut" });
    // A hidden second layer: it shows nothing, so it is no ambiguity.
    const v2 = tool(project, "add_track", { kind: "video" }).delta.created_tracks[0].track_id;
    run(["--apply-op", project, JSON.stringify({ type: "InsertClip", track_id: v2, source_id: media,
      source_in: t(0), duration: t(30), timeline_in: t(20), clip_id: "", exact_timeline: [] })]);
    tool(project, "set_track_output", { track_id: v2, visible: false, muted: false, solo: false });

    const edlBase = path.join(work, `film${rate.name}.edl`);
    run(["--export-edl", project, edlBase]);
    const edlFile = edlBase.replace(/\.edl$/, "_V1.edl");
    assert.ok(!fs.existsSync(edlBase.replace(/\.edl$/, "_V2.edl")), "a hidden track writes no list");

    const fromProject = editFromProject(readProject(project));
    const fromEdl = editFromEdl(fs.readFileSync(edlFile, "utf8"), { fps: rate.edlFps });
    const timing = (cut) => {
      const { id, from, to, ...rest } = cut;
      return rest;
    };
    assert.deepEqual(fromProject.cuts.map(timing), fromEdl.cuts.map(timing), rate.name);
    assert.deepEqual(fromProject.shots.map((s) => [s.frame, s.frames]), fromEdl.shots.map((s) => [s.frame, s.frames]));
    assert.deepEqual(fromProject.shots.slice(0, 5).map((s) => s.id), clipIds);
    for (const key of ["duration", "frames", "start", "fps", "dropFrame"])
      assert.deepEqual(fromProject[key], fromEdl[key], key);
    assert.deepEqual(fromProject.cuts.map((x) => [x.frame, x.type, x.transitionFrames]),
      [[46, "dissolve", 9], [63, "dissolve", 6], [75, "cut", 0], [136, "cut", 0], [166, "dissolve", 10]], rate.name);
    assert.equal(fromProject.cuts[0].centerFrame, 50.5);
    assert.equal(fromProject.cuts[2].gap, Math.round(5 * rate.den / rate.num * 1e6) / 1e6, "five frames, to the microsecond");
    assert.ok(fromProject.warnings.some((w) => /fades the first shot in/.test(w)), fromProject.warnings.join("; "));
    assert.ok(fromProject.warnings.some((w) => /V2 is hidden/.test(w)), fromProject.warnings.join("; "));

    for (const sampleRate of [48000, 44100]) {
      const fromP = path.join(work, `p${rate.name}-${sampleRate}.wav`);
      const fromE = path.join(work, `e${rate.name}-${sampleRate}.wav`);
      const p = render(["--project", project], sampleRate, fromP);
      const q = render(["--edl", edlFile, "--fps", rate.edlFps], sampleRate, fromE);
      const expected = fromProject.cuts.map((x) => frameToSample(x.frame, fromProject.fps, sampleRate));
      assert.deepEqual(clicks(fromP), expected, `${rate.name} at ${sampleRate}`);
      // The exact sample, by fractions: round(frame × den × sr / num), a half rounding up.
      for (const [i, x] of fromProject.cuts.entries()) {
        const twice = 2 * x.frame * rate.den * sampleRate;
        assert.equal(expected[i], Math.floor((twice + rate.num) / (2 * rate.num)));
      }
      assert.equal(p.sha256, q.sha256, `${rate.name} at ${sampleRate}: project and EDL render alike`);
      const hash = (f) => createHash("sha256").update(fs.readFileSync(f)).digest("hex");
      assert.equal(hash(fromP), hash(fromE), "the same file, header included");
      assert.equal(p.timeReference, frameToSample(fromProject.start.frame, fromProject.fps, sampleRate));

      // SN4 -- the stem, ingested, laid back where its timecode says: on the
      // timeline's first frame, over every frame of the edit; one undo.
      const before = fs.readFileSync(path.join(project, "project.cutmachine.json"));
      run(["--ingest", project, fromP]);
      const stem = run(["--describe", project]).library.find((m) => m.filename === path.basename(fromP));
      const placed = tool(project, "place_sound_stem", { media_id: stem.id }).placed;
      assert.equal(placed.first_frame, 0);
      assert.equal(placed.frames, fromProject.frames, `${rate.name} at ${sampleRate}: the stem covers the edit`);
      assert.equal(placed.track_added, true);
      tool(project, "undo", {});
      const back = JSON.parse(fs.readFileSync(path.join(project, "project.cutmachine.json"), "utf8"));
      assert.deepEqual(back.timelines, JSON.parse(before).timelines, "one undo takes the stem away");

      // SN7 -- the same render by the engine, through cutmachine-sound-host (JavaScriptCore):
      // the same report and the same file as the Node CLI.
      const viaHost = path.join(work, `h${rate.name}-${sampleRate}.wav`);
      const h = tool(project, "render_sound", { ditty, sample_rate: sampleRate, out: viaHost, triggers: true });
      assert.equal(h.sha256, p.sha256, `${rate.name} at ${sampleRate}: the engine renders what Node renders`);
      assert.equal(fs.readFileSync(viaHost).equals(fs.readFileSync(fromP)), true, "the same file, header included");
      assert.deepEqual(h.loops[0].triggers, expected);
    }

    if (keep) {
      const into = path.join(keep, `projet-${rate.name}`);
      fs.mkdirSync(into, { recursive: true });
      fs.copyFileSync(path.join(project, "project.cutmachine.json"), path.join(into, "project.cutmachine.json"));
      fs.copyFileSync(edlFile, path.join(into, "film_V1.edl"));
    }
    console.log(`ok ${rate.name}: ${fromProject.cuts.length} cuts, project = EDL, every click on its sample at 48 and 44.1 kHz`);
  }
  // SN7 -- the host alone carries the JIT entitlement, and is stopped when it runs past its time;
  // a ditty that throws is refused with its own words.
  const host = path.join(path.dirname(engine), "cutmachine-sound-host");
  const entitlements = spawnSync("codesign", ["-d", "--entitlements", "-", host], { encoding: "utf8" });
  assert.match(entitlements.stdout + entitlements.stderr, /com\.apple\.security\.cs\.allow-jit/);
  const engineRights = spawnSync("codesign", ["-d", "--entitlements", "-", engine], { encoding: "utf8" });
  assert.doesNotMatch(engineRights.stdout + engineRights.stderr, /allow-jit/, "the engine itself has no JIT entitlement");
  const project = path.join(work, "projet-25");
  const call = (args) => JSON.parse(spawnSync(engine, ["--call-tool", project, "render_sound", JSON.stringify(args)], { env, encoding: "utf8" }).stdout);
  const forever = path.join(work, "forever.js");
  fs.writeFileSync(forever, "loop(() => { for (;;) {} });");
  const started = Date.now();
  const stopped = call({ ditty: forever, timeout: 2, out: path.join(work, "forever.wav") });
  assert.equal(stopped.error, "SoundTimeout", JSON.stringify(stopped));
  assert.ok(Date.now() - started < 15000, "stopped within its time");
  const throws = path.join(work, "throws.js");
  fs.writeFileSync(throws, "loop(() => { throw new Error('boum'); });");
  const refused = call({ ditty: throws, out: path.join(work, "throws.wav") });
  assert.equal(refused.error, "SoundRenderFailed");
  assert.match(refused.detail, /loop "loop 0" failed: boum/);
  console.log("ok render_sound: same bytes as Node, timeout, refusal, allow-jit on the host only");
} finally {
  fs.rmSync(work, { recursive: true, force: true });
}
