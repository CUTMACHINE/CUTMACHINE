// SN2 -- the edit read from a CUTMACHINE project. The two fixtures were written by the engine
// (tests/sound_project_tests.mjs, CUTMACHINE_SOUND_FIXTURES), each beside the EDL it exports from
// the same timeline: five shots, a gap, dissolves centred on an odd number of frames and starting
// at their cut, fades from and to black, a hidden second video track.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { render } from '../runtime.mjs';
import { editFromEdl, editFromJson } from '../edl.mjs';
import { editFromProject, readProject } from '../project.mjs';
import { frameToSample, framesToSamplesCovering } from '../time.mjs';

const here = (p) => new URL(p, import.meta.url);
const FIXTURES = [
    { dir: 'projet-25', fps: '25' },
    { dir: 'projet-23976', fps: '23.976' },
];
const load = (dir) => readProject(here(`fixtures/${dir}`).pathname);
const copy = (x) => JSON.parse(JSON.stringify(x));

const CLICK = `
const clic = synth.def(class { constructor() { this.n = 0; } process() { return this.n++ === 0 ? 1 : 0; } });
loop(() => { for (const c of edit.cuts) at(c, () => clic.play(0, { attack: 0, release: 0.001 })); }, { name: 'clics' });
`;
const clicks = (r) => [...r.left.keys()].filter((i) => Math.abs(r.left[i]) > 0.5);
const timing = ({ id, from, to, ...rest }) => rest;

test('frames go to samples exactly, a half rounding up', () => {
    const ntsc = { num: 24000, den: 1001 };
    assert.equal(frameToSample(40, ntsc, 44100), 73574);          // 73573.5 exactly
    assert.equal(frameToSample(37, ntsc, 48000), 74074);
    assert.equal(frameToSample(50.5, { num: 25, den: 1 }, 48000), 96960);
    assert.equal(frameToSample(-1, { num: 25, den: 1 }, 44100), -1764);
    assert.equal(frameToSample(-0.5, { num: 30000, den: 1001 }, 48000), -801);   // -800.8
    assert.throws(() => frameToSample(1.25, ntsc, 48000), /neither a whole nor a half frame/);
    assert.throws(() => frameToSample(1, ntsc, 44100.5), /whole number/);
});

test('a stem covers its last frame whole: its length rounds up', () => {
    const ntsc = { num: 24000, den: 1001 };
    assert.equal(frameToSample(176, ntsc, 44100), 323723);                  // 323723.4
    assert.equal(framesToSamplesCovering(176, ntsc, 44100), 323724);
    assert.equal(framesToSamplesCovering(234, ntsc, 48000), 234 * 2002);     // exact stays exact
    const edit = editFromJson({ fps: '23.976', shots: [{ in: 0, out: 100 }, { in: 100, out: 176 }] });
    assert.equal(render({ code: CLICK, edit, sampleRate: 44100 }).left.length, 323724);
});

for (const { dir, fps } of FIXTURES) {
    test(`${dir}: the project gives the cuts of the EDL the engine exports`, () => {
        const fromProject = editFromProject(load(dir));
        const fromEdl = editFromEdl(readFileSync(here(`fixtures/${dir}/film_V1.edl`), 'utf8'), { fps });
        assert.deepEqual(fromProject.cuts.map(timing), fromEdl.cuts.map(timing));
        assert.deepEqual(fromProject.shots.map((s) => [s.frame, s.frames]), fromEdl.shots.map((s) => [s.frame, s.frames]));
        for (const key of ['duration', 'frames', 'start', 'fps', 'dropFrame']) assert.deepEqual(fromProject[key], fromEdl[key], key);
        assert.deepEqual(fromProject.cuts.map((c) => [c.frame, c.type, c.transitionFrames, c.centerFrame]),
            [[46, 'dissolve', 9, 50.5], [63, 'dissolve', 6, 66], [75, 'cut', 0, 75], [136, 'cut', 0, 136], [166, 'dissolve', 10, 171]]);
        assert.equal(fromProject.start.timecode, '01:00:00:00');
        // Shots are named by their clip, cuts by the junction of two clips.
        const project = load(dir);
        const clipIds = project.timelines[0].tracks[0].clips.map((c) => c.id);
        assert.deepEqual(fromProject.shots.slice(0, 5).map((s) => s.id), clipIds);
        assert.equal(fromProject.cuts[0].id, `${clipIds[0]}>${clipIds[1]}`);
        assert.equal(fromProject.shots[5].reel, 'BL');
        assert.equal(fromProject.timeline.rule, 'composite');
        assert.deepEqual(fromProject.titles, []);
        assert.ok(fromProject.warnings.some((w) => /fades the first shot in from #000000/.test(w)));
        assert.ok(fromProject.warnings.some((w) => /track V2 is hidden/.test(w)));
    });

    for (const sampleRate of [48000, 44100])
        test(`${dir}: every cut lands on its exact sample at ${sampleRate} Hz`, () => {
            const edit = editFromProject(load(dir));
            const r = render({ code: CLICK, edit, sampleRate });
            assert.deepEqual(clicks(r), edit.cuts.map((c) => frameToSample(c.frame, edit.fps, sampleRate)));
            assert.equal(r.left.length, framesToSamplesCovering(edit.frames, edit.fps, sampleRate));
        });
}

test('seconds rounded twice can miss a sample; a cut handed to at() does not', () => {
    const edit = editFromJson({ fps: '23.976', shots: [{ in: 0, out: 40 }, { in: 40, out: 80 }] });
    const bySeconds = CLICK.replace('at(c,', 'at(c.time,');
    assert.deepEqual(clicks(render({ code: bySeconds, edit, sampleRate: 44100 })), [73573]);
    assert.deepEqual(clicks(render({ code: CLICK, edit, sampleRate: 44100 })), [73574]);
    // centerFrame, endFrame and an offset in seconds read the same way.
    const code = `
const clic = synth.def(class { constructor() { this.n = 0; } process() { return this.n++ === 0 ? 1 : 0; } });
loop(() => {
    const c = edit.cuts[0];
    at({ frame: c.frame, offset: -0.5 }, () => clic.play(0, { attack: 0, release: 0.001 }));
    sleepUntil(edit.shots[1]);
    clic.play(0, { attack: 0, release: 0.001 });
    sleepUntil({ frame: edit.shots[1].endFrame - 1 });
    clic.play(0, { attack: 0, release: 0.001 });
}, { name: 'clics' });`;
    assert.deepEqual(clicks(render({ code, edit, sampleRate: 44100 })), [73574 - 22050, 73574, frameToSample(79, edit.fps, 44100)]);
});

// SN8 -- several video layers: the composed picture, by timeline_stats's rule.
const withV2 = (place) => {
    const project = copy(load('projet-25'));
    const sequence = project.timelines[0];
    const v2 = sequence.tracks.find((t) => t.index === 2);
    v2.visible = true;
    if (place) place(v2.clips[0], project);
    return { project, sequence, v2 };
};

test('a cutaway on V2 makes two cuts, and covers what lies under it', () => {
    // V2 holds a clip from frame 20 to 50, over A and over the dissolve that joined A to B.
    const { project, v2 } = withV2();
    const edit = editFromProject(project);
    const clip = v2.clips[0].id;
    assert.deepEqual(edit.cuts.map((c) => [c.frame, c.type]),
        [[20, 'cut'], [50, 'cut'], [63, 'dissolve'], [75, 'cut'], [136, 'cut'], [166, 'dissolve']]);
    assert.equal(edit.shots[1].id, clip);
    assert.ok(edit.warnings.some((w) => /is covered by a higher track: it is not seen, and makes no cut/.test(w)), edit.warnings.join('; '));
    assert.equal(edit.timeline.rule, 'composite');
});

test('a V1 shot a cutaway interrupts is seen twice, under two ids', () => {
    const { project, sequence } = withV2((clip) => {
        clip.timeline_in = { value: 90, rate: 25 };
        clip.duration = { value: 20, rate: 25 };
    });
    const d = sequence.tracks[0].clips[3].id;
    const edit = editFromProject(project);
    assert.deepEqual(edit.shots.map((s) => [s.frame, s.id === d ? 'D' : s.id === `${d}~2` ? 'D~2' : '·']).slice(3, 6),
        [[75, 'D'], [90, '·'], [110, 'D~2']]);
    assert.ok(edit.cuts.some((c) => c.id === `${sequence.tracks[2].clips[0].id}>${d}~2`));
});

test('a title in motion on V2 makes no cut, and is listed apart', () => {
    const { project, v2 } = withV2((clip, p) => {
        clip.composition_id = 'TITRE';
        delete clip.source_id;
        p.compositions = [{ id: 'TITRE', name: 'Titre', definition: {} }];
    });
    const edit = editFromProject(project);
    assert.deepEqual(edit.cuts.map((c) => c.frame), [46, 63, 75, 136, 166]);
    assert.deepEqual(edit.titles, [{ id: v2.clips[0].id, name: 'Titre', trackNumber: 2, frame: 20, endFrame: 50, frames: 30, start: 0.8, end: 2, duration: 1.2 }]);
});

test('a track named reads its clips alone, motion clips included', () => {
    const { project, sequence, v2 } = withV2();
    const onV2 = editFromProject(project, { track: v2.id });
    assert.equal(onV2.shots.length, 1);
    assert.equal(onV2.cuts.length, 0);
    assert.equal(onV2.timeline.trackNumber, 2);
    assert.equal(onV2.timeline.rule, 'track');
    const onV1 = editFromProject(project, { track: sequence.tracks[0].id });
    assert.deepEqual(onV1.cuts.map((c) => c.frame), [46, 63, 75, 136, 166]);
    const hidden = load('projet-25');
    assert.throws(() => editFromProject(hidden, { track: v2.id }), /track V2 .* is hidden/);
    const audio = sequence.tracks.find((t) => t.kind === 'audio');
    assert.throws(() => editFromProject(project, { track: audio.id }), /is not a video track \(its kind is audio\)/);
    assert.throws(() => editFromProject(project, { track: 'NOPE' }), /no track "NOPE"/);
});

test('a multicam angle that is not active makes no shot and no layer', () => {
    const project = copy(load('projet-25'));
    const sequence = project.timelines[0];
    const v2 = sequence.tracks.find((t) => t.index === 2);
    v2.visible = true;
    sequence.multicam_groups = [{ id: 'G', name: 'Multicam 1', active_angle_id: 'A1',
        angles: [{ id: 'A1', name: 'A', clip_id: sequence.tracks[0].clips[0].id }, { id: 'A2', name: 'B', clip_id: v2.clips[0].id }] }];
    assert.equal(editFromProject(project).cuts.length, 5);
});

test('a clip off the frame grid is refused, never rounded', () => {
    const project = copy(load('projet-25'));
    project.timelines[0].tracks[0].clips[3].timeline_in = { value: 151, rate: 50 };
    assert.throws(() => editFromProject(project), /does not start and end on whole frames of the timeline/);
    const big = copy(load('projet-25'));
    big.timelines[0].tracks[0].clips[0].duration = { value: 2 ** 60, rate: 25 };
    assert.throws(() => editFromProject(big), /not a time the document can hold/);
});

test('timelines: the active one by default, another by id, a document as well', () => {
    const project = load('projet-23976');
    const sequence = project.timelines[0];
    const other = { ...copy(sequence), id: 'OTHER', name: 'Version courte' };
    other.tracks[0].clips = other.tracks[0].clips.slice(0, 2);
    other.transitions = other.transitions.filter((t) => t.right_clip_id === other.tracks[0].clips[1].id);
    const two = { ...project, timelines: [sequence, other] };
    assert.equal(editFromProject(two).cuts.length, 5);
    assert.equal(editFromProject(two, { timeline: 'OTHER' }).cuts.length, 1);
    assert.equal(editFromProject(two, { timeline: 'OTHER' }).title, 'Version courte');
    assert.throws(() => editFromProject(two, { timeline: 'NOPE' }), /no timeline "NOPE"; it has .*« Version courte »/);
    const document = { version: 7, sequence, library: project.library };
    assert.deepEqual(editFromProject(document).cuts, editFromProject(project).cuts);
    assert.throws(() => editFromProject({}), /not a CUTMACHINE project or document/);
});

test('the start timecode moves the stem, not the cuts', () => {
    const at10 = editFromProject(load('projet-25'), { start: '10:00:00:00' });
    assert.equal(at10.start.frame, 10 * 3600 * 25);
    assert.deepEqual(at10.cuts.map((c) => c.frame), editFromProject(load('projet-25')).cuts.map((c) => c.frame));
});

test('a timeline of motion clips alone has no cut to read', () => {
    const project = copy(load('projet-25'));
    for (const clip of project.timelines[0].tracks[0].clips) { clip.composition_id = 'C'; delete clip.source_id; }
    project.compositions = [{ id: 'C', name: 'Carton', definition: {} }];
    assert.throws(() => editFromProject(project), /no picture clip: there is no cut to read \(motion clips are titles, not shots\)/);
    // Named, the track's motion clips are its shots.
    assert.equal(editFromProject(project, { track: project.timelines[0].tracks[0].id }).shots[2].name, 'Carton');
});
