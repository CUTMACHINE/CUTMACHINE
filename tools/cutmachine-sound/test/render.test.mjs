import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { createHash } from 'node:crypto';
import { render } from '../runtime.mjs';
import { editFromEdl, editFromJson, tcToFrames, framesToTc, parseFps } from '../edl.mjs';

const here = (p) => new URL(p, import.meta.url);
const demo = editFromEdl(readFileSync(here('../examples/demo.edl'), 'utf8'), { fps: 25 });
const impacts = readFileSync(here('../ditties/impacts.js'), 'utf8');
const hash = (r) => createHash('sha256').update(Buffer.from(r.left.buffer)).update(Buffer.from(r.right.buffer)).digest('hex');

const CLICK = `
const clic = synth.def(class { constructor() { this.n = 0; } process() { return this.n++ === 0 ? 1 : 0; } });
loop(() => { for (const c of edit.cuts) at(c.time, () => clic.play(0, { attack: 0, release: 0.001 })); }, { name: 'clics' });
`;
const peakOf = (...chs) => chs.reduce((m, ch) => ch.reduce((a, x) => Math.max(a, Math.abs(x)), m), 0);
const clicks = (r) => [...r.left.keys()].filter((i) => Math.abs(r.left[i]) > 0.5);

test('EDL: video shots, cuts, dissolve, audio ignored', () => {
    assert.equal(demo.shots.length, 9);
    assert.equal(demo.cuts.length, 8);
    assert.equal(demo.duration, 20.4);
    assert.equal(demo.start.timecode, '01:00:00:00');
    const d = demo.cuts[5];
    assert.equal(d.type, 'dissolve');
    assert.equal(d.transition, 1);
    assert.equal(d.center, 11.82);
    assert.equal(d.to, 'A002_C002_EXPO.MXF');
    assert.equal(demo.cuts[2].before, 0.64);
});

test('timecode: drop-frame and NTSC rates', () => {
    const df = parseFps('29.97');
    assert.equal(tcToFrames('00:01:00;02', df), 1800);
    assert.equal(tcToFrames('00:10:00;00', df), 17982);
    assert.equal(framesToTc(1800, df, true), '00:01:00;02');
    assert.equal(framesToTc(1799, df, true), '00:00:59;29');
    for (const f of [0, 1797, 1800, 17981, 17982, 107892, 123456]) assert.equal(tcToFrames(framesToTc(f, df, true), df), f);
    assert.deepEqual(parseFps('23.976'), { num: 24000, den: 1001 });
    assert.throws(() => tcToFrames('00:00:00;00', parseFps(25)), /drop-frame/);
});

test('every cut lands on its exact sample', () => {
    const r = render({ code: CLICK, edit: demo });
    assert.deepEqual(clicks(r), demo.cuts.map((c) => c.frame * 48000 / 25));
    for (const fps of ['23.976', '29.97']) {
        const edit = editFromJson({ fps, shots: [{ in: 0, out: 37 }, { in: 37, out: 101 }, { in: 101, out: 150 }] });
        const f = parseFps(fps);
        assert.deepEqual(clicks(render({ code: CLICK, edit })), [37, 101].map((fr) => Math.round(fr * 48000 * f.den / f.num)), fps);
    }
});

test('same seed, same samples; another seed, another render', () => {
    const a = render({ code: impacts, edit: demo, seed: 1 });
    const b = render({ code: impacts, edit: demo, seed: 1 });
    const c = render({ code: impacts, edit: demo, seed: 2 });
    assert.equal(hash(a), hash(b));
    assert.notEqual(hash(a), hash(c));
});

test('inputs override the ditty defaults', () => {
    const quiet = render({ code: impacts, edit: demo, inputs: { impact: 0, elan: 0, fondu: 0 } });
    assert.equal(peakOf(quiet.left, quiet.right), 0);
    assert.equal(quiet.inputs.impact, 0);
});

test('a plain Dittytoy ditty renders without an edit', () => {
    const r = render({ code: readFileSync(here('../ditties/brume.js'), 'utf8'), duration: 12, inputs: { graine: 3 } });
    const peak = peakOf(r.left, r.right);
    assert.ok(peak > 0.05 && peak <= 1, `peak ${peak}`);
    assert.equal(r.inputs.graine, 3);
});

test('a play scheduled before 0 s is pre-rolled, not dropped', () => {
    const code = `
const rampe = synth.def(class { constructor() { this.t = 0; } process() { return (this.t++ / 48000); } });
loop(() => at(-0.5, () => rampe.play(0, { attack: 0, release: 0.01, duration: second_to_tick(1) })), { name: 'rampe' });`;
    const r = render({ code, duration: 1 });
    assert.ok(Math.abs(r.left[0] - 0.5) < 1e-3, `first sample ${r.left[0]}`);
});

test('clear errors', () => {
    assert.throws(() => render({ code: CLICK, duration: 1 }), /no edit was given/);
    assert.throws(() => render({ code: 'loop(() => sleep(-1));', duration: 1 }), /sleep\(-1\)/);
    assert.throws(() => render({ code: '', duration: 0 }), /render length/);
});
