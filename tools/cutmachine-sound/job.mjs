// SN7 -- docs/roadmap/SON-2026-10.md. One render as one function of plain data.
//
// The Node CLI (cli.mjs, then worker.mjs) and the JavaScriptCore host the engine launches
// (cutmachine-sound-host, src/platform/SoundHost.mm) both call runJob: the montage read, the ditty rendered,
// the WAV encoded and the report written here, once, so that the two answer the same report and
// write the same bytes. What is left to a host is what differs between them: reading and writing
// files, the hash of the samples (node:crypto, CommonCrypto), and the clock.
//
// A job: { code, filename, partition?, partitionFile?, seed?, inputs?, sampleRate?, tail?,
//          stems?, triggers?, duration?, and one montage:
//          edl: { text, fps?, start? } | edit: <edit JSON> | project: { json, timeline?, track?, start? } }

import { render } from './runtime.mjs';
import { encodeWav } from './wav.mjs';
import { editFromEdl, editFromJson } from './edl.mjs';
import { editFromProject } from './project.mjs';
import { frameToSample } from './time.mjs';

/** The edit a job names, or null for a ditty rendered without a montage. */
export function editOf(job) {
    const given = ['edl', 'edit', 'project'].filter((k) => job[k] !== undefined && job[k] !== null);
    if (given.length > 1) throw new Error(`give one montage, not ${given.map((k) => '--' + k).join(' and ')}`);
    if (job.edl) return editFromEdl(job.edl.text, { fps: job.edl.fps, start: job.edl.start });
    if (job.edit) return editFromJson(job.edit);
    if (job.project) return editFromProject(job.project.json, { timeline: job.project.timeline, track: job.project.track, start: job.project.start });
    return null;
}

const db = (x) => (x > 0 ? Math.round(20 * Math.log10(x) * 10) / 10 : null);
const rms = (l, r) => {
    let s = 0;
    for (let i = 0; i < l.length; i++) s += l[i] * l[i] + r[i] * r[i];
    return Math.sqrt(s / (2 * Math.max(1, l.length)));
};

/**
 * Render a job. Returns the report (without `out`, `sha256` and the stems' files, which the host
 * adds), the encoded WAV and one per loop when stems were asked for.
 * @param {object} job
 * @param {{ now?: () => number }} [clock] milliseconds, for renderSeconds
 */
export function runJob(job, { now = () => Date.now() } = {}) {
    const edit = editOf(job);
    const duration = job.duration !== undefined ? Number(job.duration) : undefined;
    if (!edit && !(duration > 0)) throw new Error('render needs --edl, --edit, --project or --duration');
    const sampleRate = job.sampleRate ?? 48000;
    if (!Number.isInteger(sampleRate) || sampleRate <= 0) throw new Error(`--sr ${job.sampleRate}: expected a whole number of samples per second`);
    const timeReference = edit ? frameToSample(edit.start.frame, edit.fps, sampleRate) : 0;

    const t0 = now();
    const res = render({
        code: job.code, filename: job.filename, edit, duration, sampleRate,
        seed: job.seed ?? 1, inputs: job.inputs ?? {}, tail: job.tail ?? 0,
        stems: !!job.stems, partition: job.partition, triggers: !!job.triggers,
    });
    const renderSeconds = (now() - t0) / 1000;

    const description = `cutmachine-sound ${job.filename}${job.partitionFile ? ` partition=${job.partitionFile}` : ''} seed=${res.seed}`;
    const main = encodeWav({ left: res.left, right: res.right, sampleRate: res.sampleRate, timeReference, description });
    const stems = (res.stems ?? []).map((s, i) => {
        const wav = encodeWav({ left: s.left, right: s.right, sampleRate: res.sampleRate, timeReference, description: `${description} stem=${s.name}` });
        return { name: s.name, fileName: `${String(i + 1).padStart(2, '0')}-${s.name.replace(/[^\p{L}\p{N}_-]+/gu, '_')}.wav`, wav, peakDb: db(wav.peak) };
    });

    const report = {
        ok: true,
        sampleRate: res.sampleRate,
        duration: res.duration,
        seed: res.seed,
        inputs: res.inputs,
        loops: res.loops,
        peakDb: db(main.peak),
        rmsDb: db(rms(res.left, res.right)),
        clippedSamples: main.clipped,
        renderSeconds: Math.round(renderSeconds * 100) / 100,
        messages: res.messages,
    };
    if (edit) {
        report.edit = { cuts: edit.cuts.length, shots: edit.shots.length, start: edit.start.timecode, warnings: edit.warnings };
        if (edit.timeline) report.edit.timeline = edit.timeline;
        report.timeReference = timeReference;
    }
    return { report, main, stems };
}
