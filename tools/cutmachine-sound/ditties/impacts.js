// Impacts — sound design calé sur les coupes d'un montage.
// Ne tourne que dans cutmachine-sound (il lit l'objet `edit`), pas sur dittytoy.net.
//
// Un impact sur chaque coupe franche. Son poids dépend des plans autour :
// un long plan qui casse frappe fort, une rafale de plans courts reste légère.
// Certaines coupes après un plan assez long sont précédées d'un élan (whoosh qui y mène).
// Les fondus reçoivent un souffle qui enfle puis retombe sur toute leur durée.
//
// Chaque son est posé par sa coupe (at(c, …)), sur la frame exacte de la coupe : le module
// convertit cette frame en échantillons, la ditty ne calcule aucun instant.

input.impact = 0.85;   // min=0, max=1, step=0.01
input.grave = 0.7;     // min=0, max=1, step=0.01
input.elan = 0.4;      // min=0, max=1, step=0.01
input.fondu = 0.6;     // min=0, max=1, step=0.01
input.espace = 0.35;   // min=0, max=1, step=0.01

const TAU = Math.PI * 2;

// ─────────────────────────────────────────── lecture du montage

// Poids d'une coupe entre 0,15 et 1
const poids = (c) => clamp(0.15 + 0.6 * Math.min(1, c.before / 3) + 0.25 * Math.min(1, c.after / 2), 0.15, 1);

// ─────────────────────────────────────────── outils

class Filtre {   // SVF trapézoïdal, deux canaux ; run() rend le passe-bas, .bp et .hp restent lisibles
    constructor() {
        this.s = [0, 0, 0, 0];
        this.bp = 0;
        this.hp = 0;
        this.set(1000, 0);
    }
    set(fc, q) {
        const g = Math.tan(Math.PI * clamp(fc, 20, 0.45 / ditty.dt) * ditty.dt);
        this.k = 2 - 2 * clamp(q, 0, 0.98);
        this.a1 = 1 / (1 + g * (g + this.k));
        this.a2 = g * this.a1;
        this.a3 = g * this.a2;
    }
    run(v, c) {
        const s = this.s, i = c * 2;
        const v3 = v - s[i + 1];
        const v1 = this.a1 * s[i] + this.a2 * v3;
        const v2 = s[i + 1] + this.a2 * s[i] + this.a3 * v3;
        s[i] = 2 * v1 - s[i];
        s[i + 1] = 2 * v2 - s[i + 1];
        this.bp = v1;
        this.hp = v - this.k * v1 - v2;
        return v2;
    }
}
const bruit = () => Math.random() * 2 - 1;

// ─────────────────────────────────────────── instruments

// Impact : sub qui plonge, coup sourd dans le bas-médium, craquement, queue de bruit qui s'assombrit
const impact = synth.def(class {
    constructor(opt) {
        this.t = 0;
        this.ph = Math.random();
        this.k = 0;
        this.f0 = 40 + 8 * Math.random();
        this.corps = new Filtre();
        this.corps.set(150 + 70 * Math.random(), 0.6);
        this.craque = new Filtre();
        this.craque.set(4500, 0.3);
        this.queue = new Filtre();
    }
    process(note, env, tick, opt) {
        const t = this.t, p = opt.poids;
        this.t += ditty.dt;
        const f = this.f0 * (1 + 1.6 * Math.exp(-t / 0.045)) * (1 - 0.15 * Math.min(1, t / 1.5));
        this.ph += f * ditty.dt;
        if (this.ph >= 1) this.ph -= 1;
        const sub = Math.sin(this.ph * TAU) * Math.exp(-t / (0.25 + 0.6 * p)) * opt.grave;
        const n = bruit();
        this.corps.run(n, 0);
        const corps = this.corps.bp * Math.exp(-t / 0.06) * 2.2 * (0.5 + 0.5 * p);
        this.craque.run(n, 0);
        const craque = this.craque.hp * Math.exp(-t / 0.004) * 0.9;
        const x = Math.tanh((sub * 1.2 + corps + craque) * 1.3);
        if ((this.k++ & 15) === 0) this.queue.set(300 + 2700 * Math.exp(-t / 0.35), 0.2);
        const qe = Math.exp(-t / (0.3 + 0.5 * p)) * 0.3 * p;
        const ql = this.queue.run(bruit(), 0) * qe;
        const qr = this.queue.run(bruit(), 1) * qe;
        return [(x + ql) * env.value, (x + qr) * env.value];
    }
}, { poids: 0.5, grave: 0.7 });

// Souffle filtré qui traverse l'image.
//   forme 0 : élan qui monte jusqu'à la coupe puis s'éteint net
//   forme 1 : enfle puis retombe, pour un fondu
const whoosh = synth.def(class {
    constructor() {
        this.t = 0;
        this.k = 0;
        this.f = new Filtre();
        this.sens = Math.random() < 0.5 ? -1 : 1;
    }
    process(note, env, tick, opt) {
        const t = this.t, T = opt.long;
        this.t += ditty.dt;
        const x = Math.min(1, t / T);
        let a, fc;
        if (opt.forme === 0) {
            a = Math.pow(x, 2.2) * (t > T ? Math.exp(-(t - T) / 0.03) : 1);
            fc = 250 * Math.pow(16, Math.pow(x, 1.5));
        } else {
            const s = Math.sin(Math.PI * x);
            a = s * s;
            fc = 300 * Math.pow(8, s);
        }
        if ((this.k++ & 15) === 0) this.f.set(fc, 0.55);
        const pan = this.sens * (x * 2 - 1) * 0.7;
        this.f.run(bruit(), 0);
        const l = this.f.bp;
        this.f.run(bruit(), 1);
        const r = this.f.bp;
        a *= env.value * 1.6;
        return [l * a * (pan > 0 ? 1 - pan : 1), r * a * (pan < 0 ? 1 + pan : 1)];
    }
}, { forme: 0, long: 0.5 });

// ─────────────────────────────────────────── effets

// Réverbération : huit lignes à retard, matrice de Householder, aigus amortis
const espace = filter.def(class {
    constructor() {
        const k = 1 / ditty.dt / 44100;
        this.L = [2477, 2389, 2203, 2069, 1933, 1789, 1637, 1511].map((v) => Math.floor(v * k));
        this.b = this.L.map((n) => new Float32Array(n));
        this.p = this.L.map(() => 0);
        this.z = this.L.map(() => 0);
        this.o = this.L.map(() => 0);
        this.pre = new Float32Array(Math.floor(0.02 / ditty.dt));
        this.pp = 0;
        this.amort = 1 - Math.exp(-TAU * 4500 * ditty.dt);
        this.out = [0, 0];
    }
    process(x, opt) {
        const e = this.pre[this.pp];
        this.pre[this.pp] = (x[0] + x[1]) * 0.5;
        this.pp = this.pp + 1 < this.pre.length ? this.pp + 1 : 0;
        let s = 0;
        for (let i = 0; i < 8; i++) {
            this.o[i] = this.b[i][this.p[i]];
            s += this.o[i];
        }
        s *= 0.25;
        let l = 0, r = 0;
        for (let i = 0; i < 8; i++) {
            this.z[i] += (this.o[i] - s - this.z[i]) * this.amort;
            this.b[i][this.p[i]] = e * (i & 1 ? -0.5 : 0.5) + this.z[i] * 0.84;
            if (++this.p[i] >= this.L[i]) this.p[i] = 0;
            if (i & 1) r += this.o[i]; else l += this.o[i];
        }
        this.out[0] = x[0] + l * opt.mix * 0.4;
        this.out[1] = x[1] + r * opt.mix * 0.4;
        return this.out;
    }
}, { mix: 0.35 });

// Sortie : saturation douce, pour que les impacts empilés ne dépassent jamais 0 dBFS
const sortie = filter.def((x) => [Math.tanh(x[0]), Math.tanh(x[1])]).createShared();

// ─────────────────────────────────────────── boucles (sans sleep : elles tournent une fois)

loop(() => {
    for (const c of edit.cuts) {
        if (c.type !== 'cut') continue;
        const p = poids(c);
        at(c, () => impact.play(0, {
            poids: p, grave: () => input.grave, amp: () => input.impact * (0.3 + 0.7 * p) * 0.6,
            attack: 0, release: 0.05, duration: second_to_tick(2.5),
        }));
    }
}, { name: 'impacts' })
    .connect(espace.create({ mix: () => input.espace }))
    .connect(sortie);

loop(() => {
    for (const c of edit.cuts) {
        if (c.type !== 'cut' || c.before < 0.8 || Math.random() >= input.elan) continue;
        const long = Math.min(0.8, c.before * 0.6);
        at({ frame: c.frame, offset: -long }, () => whoosh.play(0, {
            forme: 0, long, amp: 0.25 * (0.5 + 0.5 * poids(c)),
            attack: 0, release: 0.05, duration: second_to_tick(long + 0.15),
        }));
    }
}, { name: 'elans' })
    .connect(espace.create({ mix: () => input.espace }))
    .connect(sortie);

loop(() => {
    for (const c of edit.cuts) {
        if (c.type === 'cut' || c.transition <= 0) continue;
        at(c, () => whoosh.play(0, {
            forme: 1, long: c.transition, amp: () => input.fondu * 0.35,
            attack: 0, release: 0.05, duration: second_to_tick(c.transition),
        }));
    }
}, { name: 'fondus' })
    .connect(espace.create({ mix: () => input.espace * 1.5 }))
    .connect(sortie);
