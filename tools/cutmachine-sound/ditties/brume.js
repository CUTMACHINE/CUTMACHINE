// Brume — dub techno génératif, 100 % synthèse, aucun sample.
//
// La graine choisit la tonalité, les grilles d'accords et tous les motifs :
// change-la et tu obtiens un autre morceau. Les autres curseurs jouent en direct.
//
// Forme sur 64 mesures, puis ça recommence avec de nouvelles grilles :
//   0–7   accords seuls, le filtre s'ouvre (au premier passage)
//   8–15  grosse caisse
//   16–39 basse, charleys, lancers d'écho
//   40–47 le creux : un accord tenu qui respire
//   48–63 retour

input.graine = 7;        // min=1, max=99, step=1
input.ouverture = 0.45;  // min=0, max=1, step=0.01
input.echo = 0.62;       // min=0, max=0.92, step=0.01
input.espace = 0.4;      // min=0, max=1, step=0.01
input.swing = 0.15;      // min=0, max=0.5, step=0.01
input.poussiere = 0.35;  // min=0, max=1, step=0.01

ditty.bpm = 118;

const TAU = Math.PI * 2;

// ─────────────────────────────────────────────── hasard déterministe
// Chaque boucle tourne dans son propre worker : pour que la basse et les accords
// tombent d'accord, on tire les dés à partir du numéro de mesure, pas de Math.random().
const hasard = (a, b = 0) => {
    const x = Math.sin(a * 127.1 + b * 311.7 + input.graine * 74.7) * 43758.5453;
    return x - Math.floor(x);
};
const tirer = (liste, a, b) => liste[Math.floor(hasard(a, b) * liste.length)];
const plier = (n, bas) => {
    while (n < bas) n += 12;
    while (n >= bas + 12) n -= 12;
    return n;
};

// ─────────────────────────────────────────────── harmonie
const M9 = 0, M11 = 1, MAJ9 = 2, SUS = 3;
const FORMES = [
    [0, 3, 7, 10, 14],   // m9
    [0, 3, 7, 10, 17],   // m7 add11
    [0, 4, 7, 11, 14],   // maj9
    [0, 5, 7, 10, 14],   // 9sus4
];
// [degré en demi-tons depuis la tonique, forme] — deux mesures par accord
const GRILLES = [
    [[0, M9], [0, M9], [-4, MAJ9], [-2, SUS]],
    [[0, M9], [3, MAJ9], [5, M9], [5, M11]],
    [[0, M11], [0, M11], [7, M9], [7, M9]],
    [[5, M9], [3, MAJ9], [1, MAJ9], [0, M9]],
    [[0, M9], [0, M9], [0, M9], [-2, SUS]],
    [[0, M11], [-2, MAJ9], [-4, MAJ9], [-5, SUS]],
];
const tonique = () => 48 + [0, -2, 2, -3, 3, -5, 5, -4][Math.floor(hasard(0, 91) * 8)];
const accordDe = (mesure) => {
    const grille = tirer(GRILLES, Math.floor(mesure / 16), 1);   // une grille par bloc de 16 mesures
    const [degre, forme] = grille[Math.floor(mesure / 2) % 4];
    return { racine: plier(tonique() + degre, 41), forme };
};

// Accords : positions en doubles-croches (0 à 15) dans la mesure
const MOTIFS = [
    [2, 6, 10, 14],
    [3, 10],
    [0, 3, 6, 10, 13],
    [2, 7, 12],
    [6, 14],
    [2, 5, 10, 14],
    [3, 6, 11],
];
// Basse : [position, longueur] en doubles-croches, à côté des temps de la grosse caisse
const LIGNES = [
    [[2, 2], [6, 2], [10, 2], [14, 2]],
    [[2, 1], [3, 2], [7, 2], [10, 2], [14, 1]],
    [[2, 3], [7, 1], [10, 3], [15, 1]],
    [[3, 2], [6, 1], [10, 2], [13, 2]],
];

// ─────────────────────────────────────────────── forme
const section = (mesure) => {
    const b = mesure % 64, premier = mesure < 64;
    return {
        aube: premier ? Math.min(1, (b + 1) / 12) : 1,
        grosse: (b < 8 && !premier) || (b >= 8 && b < 40) || b >= 48,
        charleys: (b >= 16 && b < 40) || b >= 52,
        rares: b >= 40 && b < 52,
        ouverts: (b >= 32 && b < 40) || b >= 56,
        basse: (b >= 16 && b < 40) || b >= 48,
        creux: b >= 40 && b < 48,
    };
};

// ─────────────────────────────────────────────── outils DSP
const polyblep = (t, dt) => {
    if (t < dt) { t /= dt; return t + t - t * t - 1; }
    if (t > 1 - dt) { t = (t - 1) / dt; return t * t + t + t + 1; }
    return 0;
};

// Filtre d'état (SVF trapézoïdal), deux canaux. run() rend le passe-bas ; .bp et .hp restent lisibles.
class Filtre {
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

// ─────────────────────────────────────────────── instruments

// L'accord : cinq notes, deux scies désaccordées par note (une à gauche, une à droite),
// un seul filtre résonant pour tout l'accord, comme une mémoire d'accord de synthé.
const accord = synth.def(class {
    constructor() {
        this.ph = [];
        this.inc = [];
        this.note = -1;
        this.t = 0;
        this.k = 0;
        this.f = new Filtre();
    }
    process(note, env, tick, opt) {
        const iv = FORMES[opt.forme];
        if (note !== this.note) {
            this.note = note;
            for (let i = 0; i < iv.length * 2; i++) {
                this.inc[i] = midi_to_hz(note + iv[i >> 1] + (i & 1 ? 0.06 : -0.06)) * ditty.dt;
                if (this.ph[i] === undefined) this.ph[i] = Math.random();
            }
        }
        let l = 0, r = 0;
        for (let i = 0; i < iv.length * 2; i++) {
            const inc = this.inc[i];
            let p = this.ph[i] + inc;
            if (p >= 1) p -= 1;
            this.ph[i] = p;
            const s = 2 * p - 1 - polyblep(p, inc);
            if (i & 1) r += s; else l += s;
        }
        if ((this.k++ & 7) === 0) {
            const fc = 110 * Math.pow(28, opt.ouverture * opt.aube) * Math.pow(2, opt.fenv * Math.exp(-this.t / 0.08));
            this.f.set(fc, opt.res);
        }
        this.t += ditty.dt;
        const g = env.value * 0.3;
        return [Math.tanh(this.f.run(l, 0) * g), Math.tanh(this.f.run(r, 1) * g)];
    }
}, { forme: 0, ouverture: 0.45, aube: 1, fenv: 2, res: 0.3 });

// Grosse caisse : sinus qui plonge, accordée sur sol (49 Hz), un soupçon de clic
const grosse = synth.def(class {
    constructor() {
        this.ph = 0;
        this.t = 0;
    }
    process(note, env, tick, opt) {
        const t = this.t;
        this.t += ditty.dt;
        const f = midi_to_hz(note) * (1 + 3.2 * Math.exp(-t / 0.03) + 0.6 * Math.exp(-t / 0.12));
        this.ph += f * ditty.dt;
        if (this.ph >= 1) this.ph -= 1;
        const corps = Math.sin(this.ph * TAU) * Math.exp(-t / 0.32);
        const clic = (Math.random() * 2 - 1) * Math.exp(-t / 0.002) * 0.35;
        return Math.tanh((corps + clic) * 1.8) * env.value * 0.8;
    }
});

// Charley : les six oscillateurs carrés de la TR-808 + du souffle, passe-haut
const METAL = [205.3, 304.4, 369.6, 522.7, 540.0, 800.0];
const charley = synth.def(class {
    constructor() {
        this.ph = METAL.map(() => Math.random());
        this.t = 0;
        this.f = new Filtre();
        this.f.set(8500, 0.2);
    }
    process(note, env, tick, opt) {
        let m = 0;
        for (let i = 0; i < 6; i++) {
            const p = this.ph[i] + METAL[i] * ditty.dt;
            this.ph[i] = p >= 1 ? p - 1 : p;
            m += this.ph[i] < 0.5 ? 1 : -1;
        }
        this.f.run(m * 0.1 + (Math.random() * 2 - 1) * 0.35, 0);
        const t = this.t;
        this.t += ditty.dt;
        return this.f.hp * Math.exp(-t / opt.decay) * env.value;
    }
}, { decay: 0.035 });

// Rimshot : un coup de bruit qui fait sonner un filtre résonant, plus un petit corps
const rim = synth.def(class {
    constructor() {
        this.t = 0;
        this.ph = 0;
        this.f = new Filtre();
        this.f.set(1700, 0.9);
    }
    process(note, env, tick, opt) {
        const t = this.t;
        this.t += ditty.dt;
        this.ph += 480 * ditty.dt;
        this.f.run((Math.random() * 2 - 1) * Math.exp(-t / 0.0015), 0);
        return (this.f.bp * 0.9 + Math.sin(this.ph * TAU) * Math.exp(-t / 0.012) * 0.5) * env.value;
    }
});

// Basse : sinus saturé, arrondi par un passe-bas
const basse = synth.def(class {
    constructor() {
        this.ph = 0;
        this.f = new Filtre();
        this.f.set(300, 0.1);
    }
    process(note, env, tick, opt) {
        this.ph += midi_to_hz(note) * ditty.dt;
        if (this.ph >= 1) this.ph -= 1;
        return this.f.run(Math.tanh(Math.sin(this.ph * TAU) * 2.2), 0) * env.value * 0.7;
    }
});

// Souffle : bruit dans un passe-bande étroit qui dérive lentement, comme du vent dans un tuyau
const souffle = synth.def(class {
    constructor() {
        this.t = Math.random() * 100;
        this.k = 0;
        this.f = new Filtre();
    }
    process(note, env, tick, opt) {
        if ((this.k++ & 15) === 0)
            this.f.set(midi_to_hz(note) * Math.pow(2, 1.3 * Math.sin(this.t * 0.11) + 0.4 * Math.sin(this.t * 0.37)), 0.8);
        this.t += ditty.dt;
        this.f.run(Math.random() * 2 - 1, 0);
        const l = this.f.bp;
        this.f.run(Math.random() * 2 - 1, 1);
        const r = this.f.bp;
        return [l * env.value * 0.3, r * env.value * 0.3];
    }
});

// ─────────────────────────────────────────────── effets

// Écho à bande ping-pong : chaque répétition s'assombrit et s'amincit, et la bande pleure un peu
const echo = filter.def(class {
    constructor() {
        this.n = Math.ceil(2 * 60 / ditty.bpm / ditty.dt) + 8;   // jusqu'à deux temps
        this.b = [new Float32Array(this.n), new Float32Array(this.n)];
        this.w = 0;
        this.t = 0;
        this.lp = [0, 0];
        this.hp = [0, 0];
        this.ahp = 1 - Math.exp(-TAU * 160 * ditty.dt);
        this.ton = -1;
        this.alp = 1;
        this.out = [0, 0];
    }
    process(x, opt) {
        if (opt.ton !== this.ton) {
            this.ton = opt.ton;
            this.alp = 1 - Math.exp(-TAU * opt.ton * ditty.dt);
        }
        const n = this.n;
        const d = opt.division * 60 / ditty.bpm / ditty.dt * (1 + 0.0015 * Math.sin(this.t * TAU * 0.7));
        this.t += ditty.dt;
        let rp = this.w - d;
        if (rp < 0) rp += n;
        const i = Math.floor(rp), f = rp - i, j = i + 1 < n ? i + 1 : 0;
        const yl = this.b[0][i] + (this.b[0][j] - this.b[0][i]) * f;
        const yr = this.b[1][i] + (this.b[1][j] - this.b[1][i]) * f;
        this.lp[0] += (yl - this.lp[0]) * this.alp;
        this.lp[1] += (yr - this.lp[1]) * this.alp;
        this.hp[0] += (this.lp[0] - this.hp[0]) * this.ahp;
        this.hp[1] += (this.lp[1] - this.hp[1]) * this.ahp;
        const fb = opt.retour;
        this.b[0][this.w] = Math.tanh((x[0] + x[1]) * 0.5 + (this.lp[1] - this.hp[1]) * fb);   // ping…
        this.b[1][this.w] = Math.tanh((this.lp[0] - this.hp[0]) * fb);                       // …pong
        this.w = this.w + 1 < n ? this.w + 1 : 0;
        this.out[0] = x[0] + yl * opt.mouille;
        this.out[1] = x[1] + yr * opt.mouille;
        return this.out;
    }
}, { division: 0.75, retour: 0.6, ton: 2200, mouille: 0.7 });

// Réverbération : réseau de huit lignes à retard (matrice de Householder), amortie dans les aigus
const espace = filter.def(class {
    constructor() {
        const k = 1 / ditty.dt / 44100;
        this.L = [2477, 2389, 2203, 2069, 1933, 1789, 1637, 1511].map(v => Math.floor(v * k));
        this.b = this.L.map(n => new Float32Array(n));
        this.p = this.L.map(() => 0);
        this.z = this.L.map(() => 0);
        this.o = this.L.map(() => 0);
        this.pre = new Float32Array(Math.floor(0.03 / ditty.dt));
        this.pp = 0;
        this.amort = 1 - Math.exp(-TAU * 5200 * ditty.dt);
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
            this.b[i][this.p[i]] = e * (i & 1 ? -0.5 : 0.5) + this.z[i] * 0.88;
            if (++this.p[i] >= this.L[i]) this.p[i] = 0;
            if (i & 1) r += this.o[i]; else l += this.o[i];
        }
        this.out[0] = x[0] + l * opt.mix * 0.4;
        this.out[1] = x[1] + r * opt.mix * 0.4;
        return this.out;
    }
}, { mix: 0.4 });

// Bus de sortie : craquements de vinyle, souffle de bande, saturation douce
const maitre = filter.def(class {
    constructor() {
        this.pop = 0;
        this.out = [0, 0];
    }
    process(x, opt) {
        const p = opt.poussiere;
        if (Math.random() < 25 * p * ditty.dt)
            this.pop += (Math.random() < 0.5 ? -1 : 1) * Math.pow(Math.random(), 2) * 0.25;
        this.pop *= 0.6;
        const sl = (Math.random() * 2 - 1) * 0.004 * p;
        const sr = (Math.random() * 2 - 1) * 0.004 * p;
        this.out[0] = Math.tanh((x[0] + this.pop + sl) * 0.75);
        this.out[1] = Math.tanh((x[1] + this.pop * 0.7 + sr) * 0.75);
        return this.out;
    }
}, { poussiere: 0.35 }).createShared({ poussiere: () => input.poussiere });

// ─────────────────────────────────────────────── boucles (une itération = une mesure)

loop((mesure) => {
    const s = section(mesure);
    const { racine, forme } = accordDe(mesure);
    const ouverture = () => input.ouverture;
    if (s.creux) {
        if (mesure % 2 === 0)
            accord.play(racine, { forme, ouverture, aube: 1.25, fenv: 0, res: 0.2, attack: 1.2, release: 2.5, duration: 6, amp: 0.4 });
        sleep(4);
        return;
    }
    const motif = tirer(MOTIFS, Math.floor(mesure / 2), 2);
    let pas = 0;
    for (const p of motif) {
        if (p > pas) sleep((p - pas) / 4);
        pas = p;
        const h = hasard(mesure * 16 + p, 3);
        accord.play(racine, { forme, ouverture, aube: s.aube, fenv: 1.4 + h * 1.6, amp: 0.6 + h * 0.3, attack: 0.002, release: 0.16, duration: 0.1 });
    }
    sleep((16 - pas) / 4);
}, { name: 'accords', amp: 1.4 })
    .connect(echo.create({ division: 0.75, retour: () => input.echo, ton: 2200, mouille: 0.75 }))
    .connect(espace.create({ mix: () => input.espace }))
    .connect(maitre);

// Lancers : de temps en temps, un accord une octave plus haut jeté dans un long écho
loop((mesure) => {
    const s = section(mesure);
    if (mesure < 8 || mesure % 4 !== 3 || hasard(mesure, 5) > (s.creux ? 0.9 : 0.5)) {
        sleep(4);
        return;
    }
    const { racine, forme } = accordDe(mesure);
    const p = tirer([6, 10, 12, 14], mesure, 6);
    sleep(p / 4);
    accord.play(racine + 12, { forme, ouverture: () => input.ouverture, fenv: 2.5, res: 0.45, amp: 0.5, attack: 0.002, release: 0.12, duration: 0.08 });
    sleep((16 - p) / 4);
}, { name: 'lancers' })
    .connect(echo.create({ division: 1.5, retour: 0.8, ton: 1500, mouille: 1 }))
    .connect(espace.create({ mix: 0.7 }))
    .connect(maitre);

loop((mesure) => {
    const s = section(mesure);
    for (let t = 0; t < 4; t++) {
        if (s.grosse) grosse.play(31, { attack: 0, release: 0.05, duration: 0.85, amp: t === 0 ? 1 : 0.92 });
        sleep(1);
    }
}, { name: 'grosse caisse', amp: 0.8 })
    .connect(maitre);

loop((mesure) => {
    const s = section(mesure);
    const sw = input.swing * 0.125;
    for (let i = 0; i < 16; i++) {
        const h = hasard(mesure * 16 + i, 7);
        const contre = i % 4 === 2;
        if (s.charleys || (s.rares && contre)) {
            const proba = contre ? 1 : (i % 2 ? 0.55 : 0.8);
            if (h < proba)
                charley.play(0, { decay: 0.03 + 0.02 * h, amp: (contre ? 0.55 : 0.28) * (0.7 + 0.3 * h), pan: 0.25, release: 0.05, duration: 0.1 });
        }
        if (s.ouverts && contre && hasard(mesure * 16 + i, 8) < 0.6)
            charley.play(0, { decay: 0.22, amp: 0.3, pan: -0.2, release: 0.05, duration: 0.6 });
        if (s.charleys && i === 12 && hasard(mesure, 9) < 0.35)
            rim.play(0, { amp: 0.3, pan: -0.4, release: 0.05, duration: 0.2 });
        if (s.charleys && i === 10 && mesure % 4 === 3)
            rim.play(0, { amp: 0.22, pan: 0.4, release: 0.05, duration: 0.2 });
        sleep(0.25 + (i % 2 ? -sw : sw));
    }
}, { name: 'charleys', amp: 2 })
    .connect(echo.create({ division: 0.75, retour: 0.35, ton: 5000, mouille: 0.35 }))
    .connect(maitre);

loop((mesure) => {
    const s = section(mesure);
    if (!s.basse) {
        sleep(4);
        return;
    }
    const r = plier(accordDe(mesure).racine, 31);
    const ligne = tirer(LIGNES, Math.floor(mesure / 4), 10);
    let pas = 0;
    for (const [p, long] of ligne) {
        if (p > pas) sleep((p - pas) / 4);
        pas = p;
        const h = hasard(mesure * 16 + p, 11);
        basse.play(r + (h < 0.15 ? 12 : h < 0.25 ? 7 : 0), { attack: 0.004, release: 0.07, duration: long / 4 * 0.8, amp: 0.8 });
    }
    sleep((16 - pas) / 4);
}, { name: 'basse', amp: 0.75 })
    .connect(maitre);

loop((n) => {
    souffle.play(tonique() + 24, { attack: 6, release: 8, duration: 30, amp: 0.35 });
    sleep(32);
}, { name: 'souffle' })
    .connect(espace.create({ mix: 0.5 }))
    .connect(maitre);
