// SN9 -- a Strudel pattern written as a partition. Needs the Strudel the build fetches: CTest gives
// its folder (CMakeLists.txt), `npm test` finds it in the repository's build, or the suite skips.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { render } from '../runtime.mjs';
import { editFromJson } from '../edl.mjs';
import { readPartition, schedulePartition } from '../partition.mjs';
import { div, exact, frameTime, toSample } from '../time.mjs';
import { compileStrudel, loadStrudel, strudelLocation } from '../strudel.mjs';

const given = process.argv[2];
let strudel = null, absent = null;
try { strudel = await loadStrudel(strudelLocation(given)); } catch (e) {
    if (given) throw e;                       // CTest named a folder: it must load
    absent = `Strudel is not fetched (${e.message})`;
}
const it = (name, fn) => test(name, { skip: absent ?? false }, fn);
const here = (p) => new URL(p, import.meta.url);
const KIT = readFileSync(here('../instruments/strudel.js'), 'utf8');
const edit10s = editFromJson({ fps: '25', shots: [{ in: 0, out: 100, id: 'a' }, { in: 100, out: 250, id: 'b' }] });
const compile = (code, options = { cycles: 1 }) => compileStrudel(strudel, code, options);
const notesOf = (partition, name) => partition.tracks.find((t) => t.name === name).notes;

it('a pattern\'s events are the notes of the partition, on the samples Strudel\'s own time gives', async () => {
    const code = 'setcpm(90/4)\n$: s("bd [sd sd sd] ~ [~ bd]")';
    const { partition, report } = await compile(code, { cycles: 2 });
    assert.equal(report.bpm, 90);
    assert.deepEqual(notesOf(partition, 'sd').map((n) => n.beat), [1, '4/3', '5/3', 5, '16/3', '17/3']);
    assert.deepEqual(notesOf(partition, 'sd').map((n) => n.beats), ['1/3', '1/3', '1/3', '1/3', '1/3', '1/3']);
    // The samples, from Strudel's own events: cycle / cps seconds, rounded once.
    const cps = exact(report.cps);
    const haps = strudel.scope.s('bd [sd sd sd] ~ [~ bd]').queryArc(0, 2).filter((h) => h.hasOnset());
    const expected = (sound) => haps.filter((h) => h.value.s === sound)
        .map((h) => toSample(div(exact(h.whole.begin.toFraction()), cps), 48000)).sort((x, y) => x - y);
    const schedule = schedulePartition(readPartition(partition), edit10s, 48000);
    for (const track of schedule) assert.deepEqual(track.items.map((i) => i.sample), expected(track.name), track.name);
    // And the kit triggers there.
    const r = render({ code: KIT, edit: edit10s, partition, triggers: true });
    assert.deepEqual(Object.fromEntries(r.loops.map((l) => [l.name, l.triggers])), { bd: expected('bd'), sd: expected('sd') });
});

it('pitches are those Strudel\'s synths play: freq, then a note name or number, then octave', async () => {
    const { partition } = await compile('$: note("c eb4 f#2 cs4 bf3 60 61.5").s("sine")\n$: freq("440 220").s("saw").octave(1)');
    assert.deepEqual(notesOf(partition, 'sine').map((n) => n.note), [48, 63, 42, 61, 58, 60, 61.5]);
    assert.deepEqual(['c', 'eb4', 'f#2', 'cs4', 'bf3'].map((x) => strudel.core.noteToMidi(x)), [48, 63, 42, 61, 58]);
    assert.deepEqual(notesOf(partition, 'saw').map((n) => n.note), [81, 69]);
    assert.equal(notesOf(partition, 'saw')[0].params, undefined, 'octave is in the pitch, not left in params');
    // A note without a sound is superdough's triangle; a hit without a pitch plays the instrument's own.
    const bare = await compile('note("a4 c5")');
    assert.deepEqual(bare.partition.tracks.map((t) => t.instrument), ['triangle']);
    assert.equal(notesOf((await compile('s("bd")')).partition, 'bd')[0].note, undefined);
});

it('labels, a preamble and the last expression: what strudel.cc plays', async () => {
    const labelled = await compile([
        '// a comment, then statements',
        'const kick = s("bd*2")',
        'setcps(1)',
        'drums: stack(kick, s("~ sd"))',
        '$: note("c2").s("saw")',
        'bass: note("<c2 g1>")',
        '  .s("square")',
        '_$: s("hh*8")',
    ].join('\n'));
    assert.deepEqual(labelled.partition.tracks.map((t) => [t.name, t.instrument]),
        [['drums bd', 'bd'], ['drums sd', 'sd'], ['saw', 'saw'], ['bass', 'square']]);
    assert.equal(labelled.report.cps, 1);
    assert.match(labelled.report.messages.join('\n'), /1 muted pattern/);
    const last = await compile('const p = s("bd sd")\nsetcpm(30)\np.fast(2);\n');
    assert.deepEqual(notesOf(last.partition, 'bd').map((n) => n.beat), [0, 2]);
    assert.equal(last.report.bpm, 120);
});

it('the same code writes the same partition: Strudel\'s randomness is the cycle\'s', async () => {
    // Two patterns: on one, degradeBy(.5) keeps the draws >= .5 and sometimesBy(.3) changes those
    // < .3 -- the same draw at the same instant, as in Tidal: chained, they never meet.
    const code = '$: s("hh*16").degradeBy(.5)\n$: s("cp*16").sometimesBy(.3, x => x.gain(.4))';
    const a = await compile(code, { cycles: 4 }), b = await compile(code, { cycles: 4 });
    assert.deepEqual(a.partition, b.partition);
    const n = notesOf(a.partition, 'hh').length;
    assert.ok(n > 10 && n < 54, `about half of 64 hats stay: ${n}`);
    const soft = notesOf(a.partition, 'cp').filter((x) => x.params?.gain === 0.4).length;
    assert.ok(soft > 5 && soft < 40, `about a third of 64 claps are softer: ${soft}`);
});

it('clip shortens, velocity is the partition\'s, the other controls are params and counted', async () => {
    const { partition, report } = await compile('note("c3 e3").s("saw").clip(.5).velocity(.7).lpf(500).room(.3)');
    assert.deepEqual(notesOf(partition, 'saw'), [
        { beat: 0, note: 48, beats: 1, velocity: 0.7, params: { cutoff: 500, room: 0.3 } },
        { beat: 2, note: 52, beats: 1, velocity: 0.7, params: { cutoff: 500, room: 0.3 } },
    ]);
    assert.deepEqual(report.controls, { cutoff: 2, room: 2 });
});

it('given an edit, the partition reaches its end from the grid\'s origin, exactly', async () => {
    // 1 cycle a second from the cut at 4 s to the end at 10 s: six cycles, nothing past the end.
    const { partition, report } = await compile('setcps(1)\n$: s("bd")', { edit: edit10s, at: { cut: 'a>b' } });
    assert.equal(report.cycles, 6);
    assert.deepEqual(partition.tempo, { bpm: 240, at: { cut: 'a>b' } });
    assert.deepEqual(schedulePartition(readPartition(partition), edit10s, 48000)[0].items.map((i) => i.sample),
        [0, 1, 2, 3, 4, 5].map((k) => (4 + k) * 48000));
    // At 23.976 fps the end is no whole cycle: the span is exact, and the last hit before it is kept.
    const ntsc = editFromJson({ fps: '24000/1001', shots: [{ in: 0, out: 100, id: 'x' }] });
    const n = await compile('setcps(1)\n$: s("bd*2")', { edit: ntsc });
    assert.equal(n.report.cycles, '1001/240');
    const end = frameTime(100, ntsc.fps);
    const hits = schedulePartition(readPartition(n.partition), ntsc, 48000)[0].items;
    assert.equal(hits.length, 9);
    assert.ok(hits.every((h) => h.sample < toSample(end, 48000)));
    assert.equal(hits[8].sample, toSample(exact(4), 48000));
});

it('refusals say what is wrong', async () => {
    await assert.rejects(compile('const a = 1'), /no pattern/);
    await assert.rejects(compile('$: 42'), /is not a pattern: 42/);
    await assert.rejects(compile('$: n("0 2").scale("C:major")'), /tonal, the sample banks and the REPL are not part of it/);
    await assert.rejects(compile('$: sequence("c e g")'), /gives "c", not a sound: write s\("…"\) or note\("…"\)/);
    await assert.rejects(compile('setcpm(0)\n$: s("bd")'), /setcpm\(0\): a tempo is one number above zero/);
    await assert.rejects(compile('$: note("h2")'), /not a note: "h2"/);
    await assert.rejects(compile('$: s("bd")', {}), /say how much to write/);
    await assert.rejects(compile('$: s("bd")', { cycles: 0 }), /cycles must be more than zero/);
    await assert.rejects(compile('$: s("bd")', { edit: edit10s, at: { edit: 'end' } }), /nothing to write/);
    await assert.rejects(compile('$: s("bd")', { edit: edit10s, at: { cut: 'z>a' } }), /the edit has no cut "z>a"/);
    // A sound the kit does not know is refused by the renderer, by name.
    const { partition } = await compile('$: s("guiro")');
    assert.throws(() => render({ code: KIT, edit: edit10s, partition }), /plays instrument "guiro", which the instruments do not define \(sine, sin, triangle/);
});

it('the kit plays every sound it names, where the partition says', async () => {
    const sounds = ['sine', 'sin', 'triangle', 'tri', 'square', 'sqr', 'sawtooth', 'saw', 'bd', 'sd', 'hh', 'oh', 'cp', 'rim'];
    // Two cycles a second: the fourteen sounds, one a cycle, in 7 s of the edit's 10.
    const { partition } = await compile(`setcps(2)\n$: note("c3").s("<${sounds.join(' ')}>").lpf(2000).pan(.25)`, { cycles: sounds.length });
    const schedule = schedulePartition(readPartition(partition), edit10s, 48000);
    const r = render({ code: KIT, edit: edit10s, partition, triggers: true, stems: true });
    assert.deepEqual(r.loops.map((l) => l.name), sounds);
    assert.deepEqual(r.loops.map((l) => l.triggers), schedule.map((t) => t.items.map((i) => i.sample)));
    assert.deepEqual(r.messages, []);
    for (const s of r.stems) {
        const peak = s.left.reduce((m, x) => Math.max(m, Math.abs(x)), 0);
        // A filtered square rings a little past its level, as Strudel's biquad does.
        assert.ok(peak > 0.05 && peak < 1.1, `${s.name} sounds, at its level: ${peak}`);
    }
});

it('the CLI writes the partition and keeps Strudel\'s greeting off its report', () => {
    const dir = mkdtempSync(join(tmpdir(), 'cm-sound-strudel-'));
    try {
        writeFileSync(join(dir, 'p.js'), 'setcpm(25)\n$: s("bd sd")');
        const cli = fileURLToPath(here('../cli.mjs'));
        const run = spawnSync(process.execPath, [cli, 'strudel', join(dir, 'p.js'), '--cycles', '2', '--out', join(dir, 'p.json'), '--strudel', strudel.dir], { encoding: 'utf8' });
        assert.equal(run.status, 0, run.stderr);
        const report = JSON.parse(run.stdout);
        assert.equal(report.ok, true);
        assert.deepEqual(report.tracks, [{ name: 'bd', instrument: 'bd', notes: 2 }, { name: 'sd', instrument: 'sd', notes: 2 }]);
        assert.equal(readPartition(JSON.parse(readFileSync(join(dir, 'p.json'), 'utf8'))).tempo.bpm.n, 100n);
    } finally { rmSync(dir, { recursive: true, force: true }); }
});
