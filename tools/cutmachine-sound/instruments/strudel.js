// Strudel — un jeu d'instruments qui répond aux noms des sons de Strudel (SN9).
//
// Une partition écrite par `cm-sound strudel` nomme ses instruments comme Strudel nomme ses sons :
// `s("bd sd")` joue les instruments « bd » et « sd », `note("c3").s("sawtooth")` l'instrument
// « sawtooth », une note sans son le « triangle ». Ce jeu en définit une poignée, en synthèse : les
// oscillateurs de Strudel (sine, triangle, square, sawtooth et leurs alias) et une batterie (bd, sd,
// hh, oh, cp, rim). Les batteries de Strudel sont des échantillons, qu'une ditty ne lit pas : ici,
// mêmes notes, mêmes instants, pas le même son.
//
// Ce que les instruments lisent des contrôles de Strudel, avec ses valeurs par défaut :
//   gain (0,8, multiplié par velocity), attack, decay, sustain, release (la règle et les défauts
//   des synthés de Strudel : 0,001 / 0,05 / 0,6 / 0,01), cutoff et resonance (passe-bas, `lpf` et
//   `lpq` dans Strudel), pan (0 à gauche, 0,5 au centre, 1 à droite).
// Les autres contrôles (room, delay, n…) arrivent dans les paramètres et ne changent rien ; le
// rapport de `cm-sound strudel` les compte.

const TAU = Math.PI * 2;
const bruit = () => Math.random() * 2 - 1;

// Filtre d'état trapézoïdal ; run() rend le passe-bas, .bp et .hp restent lisibles.
class SVF {
    constructor() { this.s1 = 0; this.s2 = 0; this.bp = 0; this.hp = 0; this.set(1000, 0.707); }
    set(fc, q) {
        const g = Math.tan(Math.PI * clamp(fc, 20, 0.45 / ditty.dt) * ditty.dt);
        this.k = 1 / Math.max(0.1, q);
        this.a1 = 1 / (1 + g * (g + this.k));
        this.a2 = g * this.a1;
        this.a3 = g * this.a2;
    }
    run(v) {
        const v3 = v - this.s2;
        const v1 = this.a1 * this.s1 + this.a2 * v3;
        const v2 = this.s2 + this.a2 * this.s1 + this.a3 * v3;
        this.s1 = 2 * v1 - this.s1;
        this.s2 = 2 * v2 - this.s2;
        this.bp = v1;
        this.hp = v - this.k * v1 - v2;
        return v2;
    }
}

// Correction polyBLEP des discontinuités : des dents de scie et des carrés sans repliement audible.
const blep = (t, dt) => (t < dt ? (t /= dt, t + t - t * t - 1) : t > 1 - dt ? (t = (t - 1) / dt, t * t + t + t + 1) : 0);

const FORMES = {
    sine: (ph) => Math.sin(TAU * ph),
    triangle: (ph) => 1 - 4 * Math.abs(ph - 0.5),
    sawtooth: (ph, dt) => 2 * ph - 1 - blep(ph, dt),
    square: (ph, dt) => (ph < 0.5 ? 1 : -1) + blep(ph, dt) - blep((ph + 0.5) % 1, dt),
};

// Un oscillateur de Strudel, et son passe-bas quand la note a un cutoff.
const oscillateur = synth.def(class {
    constructor(opt) {
        this.ph = 0;
        this.forme = FORMES[opt.forme];
        this.filtre = opt.cutoff ? new SVF() : null;
        if (this.filtre) this.filtre.set(opt.cutoff, opt.resonance ?? 1);
    }
    process(note, env) {
        const dt = midi_to_hz(note) * ditty.dt;
        this.ph += dt;
        if (this.ph >= 1) this.ph -= 1;
        const v = this.forme(this.ph, dt);
        return (this.filtre ? this.filtre.run(v) : v) * env.value;
    }
}, { forme: 'triangle' });

// L'enveloppe des synthés de Strudel (superdough, getADSRValues) : sans réglage, ses défauts ;
// avec une attaque seule, ou sans attaque ni décroissance, le maintien est plein.
function enveloppe(p) {
    const { attack: a, decay: d, sustain: s, release: r } = p;
    if (a == null && d == null && s == null && r == null) return { attack: 0.001, decay: 0.05, sustain: 0.6, release: 0.01 };
    const sustain = s != null ? s : (a != null && d == null) || (a == null && d == null) ? 1 : 0.001;
    return { attack: Math.max(a ?? 0, 0.001), decay: Math.max(d ?? 0, 0.001), sustain: Math.min(sustain, 1), release: Math.max(r ?? 0, 0.01) };
}

const niveau = (p, ev) => (p.gain ?? 0.8) * (ev.velocity ?? 1);
const panoramique = (p) => (p.pan === undefined ? 0 : clamp(2 * p.pan - 1, -1, 1));

for (const [nom, forme] of [['sine', 'sine'], ['sin', 'sine'], ['triangle', 'triangle'], ['tri', 'triangle'],
    ['square', 'square'], ['sqr', 'square'], ['sawtooth', 'sawtooth'], ['saw', 'sawtooth']]) {
    // La note par défaut des synthés de Strudel, sans note ni fréquence : 36, un do grave.
    instrument(nom, oscillateur, {
        note: 36,
        play: (p, ev) => ({ forme, cutoff: p.cutoff, resonance: p.resonance, amp: niveau(p, ev), pan: panoramique(p), ...enveloppe(p) }),
    });
}

// ─────────────────────────────────────────── la batterie

// Grosse caisse : un sinus qui plonge de 150 à 50 Hz, un clic à l'attaque.
const bd = synth.def(class {
    constructor() { this.t = 0; this.ph = 0; }
    process() {
        const t = this.t; this.t += ditty.dt;
        this.ph += (50 + 100 * Math.exp(-t / 0.03)) * ditty.dt;
        return Math.sin(TAU * this.ph) * Math.exp(-t / 0.35) + bruit() * Math.exp(-t / 0.002) * 0.3;
    }
});

// Caisse claire : un corps à 185 Hz et un timbre de bruit passé en bande.
const sd = synth.def(class {
    constructor() { this.t = 0; this.ph = 0; this.f = new SVF(); this.f.set(3500, 0.7); }
    process() {
        const t = this.t; this.t += ditty.dt;
        this.ph += 185 * ditty.dt;
        this.f.run(bruit());
        return Math.sin(TAU * this.ph) * Math.exp(-t / 0.05) * 0.6 + this.f.bp * Math.exp(-t / 0.12) * 1.4;
    }
});

// Charleston : du bruit passé en haut, court fermé (hh), long ouvert (oh).
const charleston = synth.def(class {
    constructor(opt) { this.t = 0; this.f = new SVF(); this.f.set(8000, 0.9); this.tenue = opt.tenue; }
    process() {
        const t = this.t; this.t += ditty.dt;
        this.f.run(bruit());
        return this.f.hp * Math.exp(-t / this.tenue) * 0.5;
    }
}, { tenue: 0.04 });

// Clap : trois éclats de bruit rapprochés, puis une queue.
const cp = synth.def(class {
    constructor() { this.t = 0; this.f = new SVF(); this.f.set(1200, 1.2); }
    process() {
        const t = this.t; this.t += ditty.dt;
        this.f.run(bruit());
        const eclats = [0, 0.011, 0.022].reduce((a, d) => a + (t >= d ? Math.exp(-(t - d) / 0.006) : 0), 0);
        return this.f.bp * (eclats * 0.8 + Math.exp(-t / 0.15) * 0.5) * 1.6;
    }
});

// Rimshot : deux partiels secs.
const rim = synth.def(class {
    constructor() { this.t = 0; this.a = 0; this.b = 0; }
    process() {
        const t = this.t; this.t += ditty.dt;
        this.a += 1700 * ditty.dt; this.b += 460 * ditty.dt;
        return (Math.sin(TAU * this.a) * 0.5 + Math.sin(TAU * this.b)) * Math.exp(-t / 0.018) * 0.7;
    }
});

// Une frappe sort vers -6 dBFS à gain 1, le niveau d'un échantillon normalisé de Strudel ; les
// oscillateurs, eux, montent à 1 comme les siens.
const frappe = (tenue) => (p, ev) => ({ amp: niveau(p, ev) * 0.5, pan: panoramique(p), attack: 0, release: 0.01, duration: second_to_tick(tenue) });
instrument('bd', bd, { play: frappe(0.8) });
instrument('sd', sd, { play: frappe(0.4) });
instrument('hh', charleston, { play: (p, ev) => ({ ...frappe(0.2)(p, ev), tenue: 0.04 }) });
instrument('oh', charleston, { play: (p, ev) => ({ ...frappe(0.9)(p, ev), tenue: 0.25 }) });
instrument('cp', cp, { play: frappe(0.5) });
instrument('rim', rim, { play: frappe(0.15) });
