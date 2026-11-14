// SN5 -- the partition and the instruments apart: what plays when as data, how it sounds as code.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { createHash } from 'node:crypto';
import { render } from '../runtime.mjs';
import { editFromEdl, editFromJson } from '../edl.mjs';
import { editFromProject, readProject } from '../project.mjs';
import { PARTITION_FORMAT, formatPartition, noteNumber, readPartition, schedulePartition } from '../partition.mjs';
import { frameToSample } from '../time.mjs';
import { impactsPartition } from '../partitions/impacts.mjs';

const here = (p) => new URL(p, import.meta.url);
const read = (p) => readFileSync(here(p), 'utf8');
const demo = editFromEdl(read('../examples/demo.edl'), { fps: 25 });
const partition = JSON.parse(read('../partitions/impacts-demo.json'));
const SETS = { impacts: read('../instruments/impacts.js'), verre: read('../instruments/verre.js') };
const hash = (r) => createHash('sha256').update(Buffer.from(r.left.buffer)).update(Buffer.from(r.right.buffer)).digest('hex');
const triggers = (r) => Object.fromEntries(r.loops.map((l) => [l.name, l.triggers]));

// An instrument an agent writes from nothing: a click, played as the partition says.
const CLICKS = `
const clic = synth.def(class { constructor() { this.n = 0; } process() { return this.n++ === 0 ? 1 : 0; } });
for (const name of ['impact', 'elan', 'fondu', 'basse'])
    instrument(name, clic, { play: () => ({ attack: 0, release: 0.001, duration: 0.01 }) });
`;
const clicks = (r) => [...r.left.keys()].filter((i) => Math.abs(r.left[i]) > 0.5);

test('impacts.js split in a partition and its instruments renders the same samples', () => {
    const original = render({ code: read('../ditties/impacts.js'), edit: demo, triggers: true });
    const split = render({ code: SETS.impacts, edit: demo, partition, triggers: true });
    assert.equal(hash(split), hash(original));
    assert.deepEqual(triggers(split), triggers(original));
    assert.deepEqual(split.messages, []);
});

test('the committed partition is what partitions/impacts.mjs writes for demo.edl', () => {
    assert.equal(read('../partitions/impacts-demo.json'), formatPartition(impactsPartition(demo)));
});

test('changing the instruments moves no trigger, to the sample', () => {
    const expected = {
        impacts: demo.cuts.filter((c) => c.type === 'cut').map((c) => frameToSample(c.frame, demo.fps, 48000)),
        elans: [frameToSample(demo.cuts[0].frame, demo.fps, 48000) - 0.8 * 48000],
        fondus: [frameToSample(demo.cuts[5].frame, demo.fps, 48000)],
    };
    const runs = Object.entries(SETS).map(([name, code]) => [name, render({ code, edit: demo, partition, triggers: true })]);
    for (const [name, r] of runs) assert.deepEqual(triggers(r), expected, name);
    assert.notEqual(hash(runs[0][1]), hash(runs[1][1]), 'another set sounds otherwise');
    // And an instrument written from nothing hears the partition on the same samples.
    const own = render({ code: CLICKS, edit: demo, partition, triggers: true });
    assert.deepEqual(triggers(own), expected);
});

test('on a project at 23.976 fps and 44.1 kHz, every set triggers on the exact samples', () => {
    const edit = editFromProject(readProject(here('fixtures/projet-23976').pathname));
    const p = impactsPartition(edit);
    const exact = edit.cuts.filter((c) => c.type === 'cut').map((c) => frameToSample(c.frame, edit.fps, 44100));
    const seen = [];
    for (const code of [SETS.impacts, SETS.verre, CLICKS]) {
        const r = render({ code, edit, partition: p, sampleRate: 44100, triggers: true });
        assert.deepEqual(triggers(r).impacts, exact);
        seen.push(triggers(r));
    }
    assert.deepEqual(seen[1], seen[0]);
    assert.deepEqual(seen[2], seen[0]);
});

test('anchors: a cut and its transition, a shot, the edit, offsets, leads and lengths', () => {
    // In the simple form, as in an EDL, a dissolve belongs to its incoming shot and starts on its in.
    const edit = editFromJson({ fps: '23.976', shots: [
        { in: 0, out: 40, id: 'a' }, { in: 40, out: 81, id: 'b' }, { in: 81, out: 120, id: 'c', transition: { type: 'dissolve', frames: 9 } }] });
    const at = (anchor, more = {}) => ({ format: PARTITION_FORMAT, version: 1, tracks: [{ instrument: 'x', events: [{ at: anchor, ...more }] }] });
    const one = (p, sr = 44100) => schedulePartition(readPartition(p), edit, sr)[0].items[0];
    const S = (frame) => frameToSample(frame, edit.fps, 44100);
    assert.equal(one(at({ cut: 'a>b' })).sample, S(40));
    assert.equal(one(at({ cut: 'b>c', point: 'start' })).sample, S(81));
    assert.equal(one(at({ cut: 'b>c', point: 'center' })).sample, S(85.5));
    assert.equal(one(at({ cut: 'b>c', point: 'end' })).sample, S(90));
    assert.equal(one(at({ shot: 'b', point: 'end' })).sample, S(81));
    assert.equal(one(at({ shot: 'c', point: 'center' })).sample, S(100.5));
    assert.equal(one(at({ edit: 'end' })).sample, S(120));
    // Offsets and leads are added to the exact time, then rounded once; "1/10" is a tenth exactly.
    assert.equal(one(at({ cut: 'a>b', offset: '1/10' })).sample, Math.floor((40 * 1001 * 44100 / 24000 + 4410) + 0.5));
    const led = one(at({ cut: 'a>b' }, { lead: 0.5 }));
    assert.equal(led.sample, Math.floor(40 * 1001 * 44100 / 24000 - 22050 + 0.5));
    assert.equal(led.facts.lead, 0.5);
    assert.equal(led.facts.anchor, 40 * 1001 / 24000);
    assert.equal(one(at({ cut: 'b>c' }, { length: 'transition' })).facts.length, 9 * 1001 / 24000);
    assert.equal(one(at({ shot: 'c' }, { length: 'shot' })).facts.length, 39 * 1001 / 24000);
    assert.equal(one(at({ cut: 'a>b' }, { length: { cut: 'b>c', point: 'end' } })).facts.length, 50 * 1001 / 24000);
    assert.equal(one(at({ cut: 'a>b' })).facts.cut.id, 'a>b');
});

test('notes on a tempo grid anchored to a cut, triplets exact', () => {
    const edit = editFromJson({ fps: '25', shots: [{ in: 0, out: 50, id: 'a' }, { in: 50, out: 200, id: 'b' }] });
    const p = {
        format: PARTITION_FORMAT, version: 1, tempo: { bpm: 90, at: { cut: 'a>b' } },
        tracks: [{ name: 'basse', instrument: 'basse', notes: [
            { beat: 0, note: 'c2', beats: 1 }, { beat: '1/3', note: 'eb2' }, { beat: '2/3', note: 'g2', velocity: 0.6 }, { beat: -1, note: 36 }] }],
    };
    const items = schedulePartition(readPartition(p), edit, 48000)[0].items;
    // 90 bpm: a beat is 2/3 s = 32000 samples; the grid starts on the cut at 2 s.
    assert.deepEqual(items.map((i) => i.sample), [96000, 96000 + 10667, 96000 + 21333, 96000 - 32000]);
    assert.deepEqual(items.map((i) => i.note), [36, 39, 43, 36]);
    assert.equal(items[0].facts.length, 2 / 3);
    assert.equal(items[2].facts.velocity, 0.6);
    const r = render({ code: CLICKS, edit, partition: p, triggers: true });
    assert.deepEqual(r.loops[0].triggers, [64000, 96000, 106667, 117333]);
    assert.deepEqual(clicks(r), [64000, 96000, 106667, 117333]);
});

test('a note without a pitch plays the instrument\'s own note, like an event', () => {
    const edit = editFromJson({ fps: '25', shots: [{ in: 0, out: 100, id: 'a' }] });
    const code = `
const s = synth.def(class { process(note) { if (!this.heard) { this.heard = true; debug.log(String(note)); } return 0; } });
instrument('caisse', s, { note: 38, play: () => ({ duration: 0.01 }) });
instrument('basse', s, { play: () => ({ duration: 0.01 }) });`;
    const p = { format: PARTITION_FORMAT, version: 1, tempo: { bpm: 120 },
        tracks: [{ instrument: 'caisse', notes: [{ beat: 1 }, { beat: 3, velocity: 0.5 }] }, { instrument: 'basse', notes: [{ beat: 0 }] }] };
    const [caisse, basse] = schedulePartition(readPartition(p), edit, 48000);
    assert.deepEqual(caisse.items.map((i) => [i.sample, i.note]), [[24000, undefined], [72000, undefined]]);
    assert.equal(basse.items[0].note, undefined);
    const r = render({ code, edit, partition: p, triggers: true });
    assert.deepEqual(r.loops.map((l) => l.triggers), [[24000, 72000], [0]]);
    // What the voices heard (messages are kept once each): the instrument's note, 38, and the
    // default of an instrument without one.
    assert.deepEqual(r.messages.map((m) => m.text).sort(), ['0', '38']);
});

test('an instrument is told the free parameters and the facts of each event', () => {
    const code = `
const s = synth.def(() => 0);
instrument('x', s, { play: (p, ev) => { debug.log(JSON.stringify({ p, lead: ev.lead, length: ev.length, cut: ev.cut && ev.cut.id, track: ev.track })); return {}; } });`;
    const p = { format: PARTITION_FORMAT, version: 1, tracks: [{ name: 'piste', instrument: 'x', params: { a: 1, b: 2 },
        events: [{ at: { cut: 'shot-1>shot-2' }, lead: 0.25, length: 1, params: { b: 3, libre: 'oui' } }] }] };
    const r = render({ code, edit: demo, partition: p });
    assert.deepEqual(JSON.parse(r.messages[0].text), { p: { a: 1, b: 3, libre: 'oui' }, lead: 0.25, length: 1, cut: 'shot-1>shot-2', track: 'piste' });
});

test('refusals name what is wrong', () => {
    const base = { format: PARTITION_FORMAT, version: 1, tracks: [{ instrument: 'impact', events: [{ at: { cut: 'shot-1>shot-2' } }] }] };
    const bad = (patch) => ({ ...base, ...patch });
    assert.throws(() => readPartition(bad({ format: 'x' })), /\.format: must be "cutmachine-partition"/);
    assert.throws(() => readPartition(bad({ tempi: 1 })), /\.tempi: is not a key of the format/);
    assert.throws(() => readPartition(bad({ tracks: [{ instrument: 'i', events: [{ at: { cut: 'a', pont: 'x' } }] }] })), /\.tracks\[0\]\.events\[0\]\.at\.pont/);
    assert.throws(() => readPartition(bad({ tracks: [{ instrument: 'i', events: [{ at: { cut: 'a', shot: 'b' } }] }] })), /names one of "cut", "shot" or "edit"/);
    assert.throws(() => readPartition(bad({ tracks: [{ instrument: 'i', notes: [{ beat: 0, note: 'c2' }] }] })), /a note needs a tempo/);
    assert.throws(() => readPartition(bad({ tracks: [{ instrument: 'i', events: [{ at: { edit: 'start' }, lead: -1 }] }] })), /lead: cannot be negative/);
    assert.throws(() => noteNumber('h2', 'n'), /is not a note/);
    assert.equal(noteNumber('f#2', 'n'), 42);
    assert.equal(noteNumber('cs4', 'n'), 61);
    // An id the edit does not have: refused, never moved to another cut.
    const other = editFromJson({ fps: '25', shots: [{ in: 0, out: 10, id: 'x' }, { in: 10, out: 20, id: 'y' }] });
    assert.throws(() => render({ code: CLICKS, edit: other, partition }), /no cut "shot-1>shot-2" \(cm-sound edit lists its cuts, such as "x>y"\)/);
    assert.throws(() => render({ code: CLICKS, edit: demo, partition: bad({ tracks: [{ instrument: 'impact', events: [{ at: { cut: 'shot-2' } }] }] }) }),
        /"shot-2" is a shot, not a cut: write \{ "shot": "shot-2" \}/);
    assert.throws(() => render({ code: CLICKS, edit: demo, partition: bad({ tracks: [{ instrument: 'violon', events: [{ at: { edit: 'start' } }] }] }) }),
        /plays instrument "violon", which the instruments do not define \(impact, elan, fondu, basse\)/);
    assert.throws(() => render({ code: CLICKS, partition, duration: 2 }), /a partition plays on an edit/);
    const late = render({ code: CLICKS, edit: demo, partition: bad({ tracks: [{ instrument: 'impact', events: [{ at: { edit: 'end' }, }] }] }) });
    assert.match(late.messages[0].text, /1 of its plays start after the end and are not heard/);
    assert.match(render({ code: CLICKS, edit: demo }).messages[0].text, /defines instruments .* but no partition plays them/);
});
