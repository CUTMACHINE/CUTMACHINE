// Verre — un second jeu d'instruments pour la partition d'impacts.
//
// Mêmes noms que instruments/impacts.js (impact, elan, fondu), autre matière : les impacts sont des
// cloches de verre inharmoniques plutôt que des coups sourds, l'élan une montée de dents de scie
// accordée plutôt qu'un souffle, le fondu une nappe d'accord plutôt qu'un bruit filtré, et l'espace
// un écho ping-pong plutôt qu'une réverbération. La partition ne change pas : chaque son part sur le
// même échantillon qu'avec impacts.js.

input.verre = 0.8;     // min=0, max=1, step=0.01
input.montee = 0.5;    // min=0, max=1, step=0.01
input.nappe = 0.5;     // min=0, max=1, step=0.01
input.echo = 0.3;      // min=0, max=1, step=0.01

const TAU = Math.PI * 2;
const bruit = () => Math.random() * 2 - 1;

// Passe-bas à un pôle, deux canaux
class PasseBas {
    constructor() { this.z = [0, 0]; this.a = 1; }
    set(fc) { this.a = 1 - Math.exp(-TAU * clamp(fc, 20, 0.45 / ditty.dt) * ditty.dt); }
    run(x, c) { this.z[c] += (x - this.z[c]) * this.a; return this.z[c]; }
}

// ─────────────────────────────────────────── instruments

// Cloche : trois paires FM aux rapports inharmoniques ; plus le poids est fort, plus elle est grave
// et longue. Une pichenette de bruit aigu en attaque.
const cloche = synth.def(class {
    constructor(opt) {
        this.t = 0;
        this.f = opt.hauteur;
        this.ph = [Math.random(), Math.random(), Math.random()];
        this.pm = [0, 0, 0];
        this.r = [1, 2.756, 5.404];
        this.clic = new PasseBas();
        this.clic.set(9000);
    }
    process(note, env, tick, opt) {
        const t = this.t;
        this.t += ditty.dt;
        let l = 0, r = 0;
        for (let i = 0; i < 3; i++) {
            const fc = this.f * this.r[i];
            this.pm[i] += fc * 1.41 * ditty.dt;
            this.ph[i] += fc * ditty.dt;
            const indice = 2.2 * Math.exp(-t / (0.08 + 0.1 * i));
            const s = Math.sin(TAU * this.ph[i] + indice * Math.sin(TAU * this.pm[i]));
            const a = Math.exp(-t / (opt.tenue / (1 + i * 1.7))) / (1 + i);
            l += s * a * (i === 1 ? 0.6 : 1);
            r += s * a * (i === 2 ? 0.6 : 1);
        }
        const c = this.clic.run(bruit(), 0) * Math.exp(-t / 0.003) * 0.6;
        return [(l * 0.45 + c) * env.value, (r * 0.45 + c) * env.value];
    }
}, { hauteur: 440, tenue: 1.2 });

// Montée : cinq dents de scie désaccordées qui glissent d'une octave et demie vers l'ancre, filtre
// qui s'ouvre, coupure nette à l'arrivée.
const montee = synth.def(class {
    constructor(opt) {
        this.t = 0;
        this.ph = [0, 0, 0, 0, 0].map(() => Math.random());
        this.d = [-0.12, -0.05, 0, 0.06, 0.11];
        this.f = new PasseBas();
    }
    process(note, env, tick, opt) {
        const t = this.t, T = opt.long;
        this.t += ditty.dt;
        const x = Math.min(1, t / T);
        const f0 = midi_to_hz(note) * Math.pow(2, 1.5 * x * x);
        let l = 0, r = 0;
        for (let i = 0; i < 5; i++) {
            this.ph[i] += f0 * Math.pow(2, this.d[i] / 12) * ditty.dt;
            if (this.ph[i] >= 1) this.ph[i] -= 1;
            const s = this.ph[i] * 2 - 1;
            if (i < 3) l += s; if (i > 1) r += s;
        }
        this.f.set(200 + 6000 * x * x);
        const a = Math.pow(x, 1.6) * (t > T ? Math.exp(-(t - T) / 0.015) : 1) * env.value * 0.22;
        return [this.f.run(l, 0) * a, this.f.run(r, 1) * a];
    }
}, { long: 0.5 });

// Nappe : un accord de neuvième mineure en sinus, qui enfle puis retombe sur la durée du fondu.
const nappe = synth.def(class {
    constructor(opt) {
        this.t = 0;
        this.notes = [0, 3, 7, 10, 14].map((n) => midi_to_hz(opt.base + n));
        this.ph = this.notes.map(() => Math.random());
    }
    process(note, env, tick, opt) {
        const t = this.t;
        this.t += ditty.dt;
        const x = Math.min(1, t / opt.long);
        const s = Math.sin(Math.PI * x);
        let l = 0, r = 0;
        for (let i = 0; i < this.notes.length; i++) {
            this.ph[i] += this.notes[i] * (1 + 0.002 * Math.sin(TAU * 0.3 * t + i)) * ditty.dt;
            const v = Math.sin(TAU * this.ph[i]);
            if (i & 1) r += v; else l += v;
        }
        const a = s * s * env.value * 0.16;
        return [l * a, r * a];
    }
}, { base: 57, long: 1 });

// ─────────────────────────────────────────── effets

// Écho ping-pong amorti : gauche vers droite, droite vers gauche
const echo = filter.def(class {
    constructor() {
        this.n = Math.floor(0.27 / ditty.dt);
        this.b = [new Float32Array(this.n), new Float32Array(this.n)];
        this.p = 0;
        this.lp = new PasseBas();
        this.lp.set(3500);
        this.out = [0, 0];
    }
    process(x, opt) {
        const p = this.p;
        const dl = this.b[0][p], dr = this.b[1][p];
        this.b[0][p] = this.lp.run(x[1] + dr * 0.45, 0) * 0.9;
        this.b[1][p] = this.lp.run(x[0] + dl * 0.45, 1) * 0.9;
        this.p = p + 1 < this.n ? p + 1 : 0;
        this.out[0] = x[0] + dl * opt.mix;
        this.out[1] = x[1] + dr * opt.mix;
        return this.out;
    }
}, { mix: 0.3 });

const sortie = filter.def((x) => [Math.tanh(x[0]), Math.tanh(x[1])]).createShared();

// ─────────────────────────────────────────── ce que la partition appelle

// Le poids d'une coupe choisit la hauteur de la cloche dans une gamme pentatonique : lourd, grave.
const hauteurs = [76, 74, 71, 69, 67, 64, 62, 59].map(midi_to_hz);
instrument('impact', cloche, {
    play: (p, ev) => ({
        hauteur: hauteurs[Math.min(hauteurs.length - 1, Math.floor(p.poids * hauteurs.length))],
        tenue: 0.6 + 1.6 * p.poids, pan: (ev.index & 1 ? 0.35 : -0.35),
        amp: () => input.verre * (0.35 + 0.65 * p.poids) * 0.5,
        attack: 0, release: 0.1, duration: second_to_tick(3),
    }),
    chain: () => [echo.create({ mix: () => input.echo }), sortie],
});

instrument('elan', montee, {
    note: 45,
    play: (p, ev) => ({
        long: ev.lead, amp: () => input.montee * (0.5 + 0.5 * p.poids),
        attack: 0, release: 0.02, duration: second_to_tick(ev.lead + 0.1),
    }),
    chain: () => [echo.create({ mix: () => input.echo * 0.5 }), sortie],
});

instrument('fondu', nappe, {
    play: (p, ev) => ({
        long: ev.length, base: 57, amp: () => input.nappe,
        attack: 0, release: 0.3, duration: second_to_tick(ev.length),
    }),
    chain: () => [echo.create({ mix: () => input.echo }), sortie],
});
