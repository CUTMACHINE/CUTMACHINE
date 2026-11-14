// MOTION-WEB-2026-10 MW4 -- runs inside the page (Chromium). Reads one
// HyperFrames composition as data: its DOM as a tree of the motion format
// (flexbox kept as flexbox), its GSAP timeline as keys sampled at each
// tween's ends, with the curve GSAP names. Nothing here guesses an
// intention: what has no equivalent is listed in `notes`.
// Exposes window.__cmExtract(options) -> {composition, notes, stats}.
(() => {
  const CONTROL = new Set(["duration", "ease", "delay", "stagger", "repeat", "yoyo", "repeatDelay",
    "immediateRender", "overwrite", "paused", "id", "data", "lazy", "callbackScope", "runBackwards",
    "startAt", "inherit", "keyframes", "defaults", "onComplete", "onStart", "onUpdate", "onRepeat",
    "onReverseComplete", "onReverseCompleteParams", "onRepeatParams", "onInterrupt", "onInterruptParams",
    "onCompleteParams", "onStartParams", "onUpdateParams", "yoyoEase", "smoothChildTiming",
    "autoRemoveChildren", "parent", "reversed", "force3D", "clearProps", "autoRound", "modifiers",
    "transformOrigin", "svgOrigin", "smoothOrigin"]);
  // GSAP property -> the motion format's.
  const PROPERTY = {
    x: "translateX", xPercent: "translateX", y: "translateY", yPercent: "translateY",
    scale: "scale", scaleX: "scaleX", scaleY: "scaleY", rotation: "rotate", rotate: "rotate",
    rotationZ: "rotate", rotationX: "rotateX", rotationY: "rotateY", opacity: "opacity",
    autoAlpha: "opacity", visibility: "opacity", color: "color", backgroundColor: "backgroundColor",
    width: "width", height: "height", letterSpacing: "letterSpacing", fontSize: "fontSize",
    top: "top", left: "left", right: "right", bottom: "bottom", fill: "fill", stroke: "stroke",
    strokeWidth: "strokeWidth", clipPath: "clipPath", filter: "filter",
  };
  // MW4 -- the dash keys GSAP tweens (as CSS, or as attributes under
  // `attr`), read on the SVG shapes they reach.
  const DASH = new Set(["strokeDashoffset", "strokeDasharray", "stroke-dashoffset", "stroke-dasharray"]);
  const SHAPES = "path, line, polyline, polygon, circle, rect, ellipse";
  // Properties a box that wraps a text hands down to that text.
  const TEXT_PROPS = new Set(["color", "letterSpacing", "fontSize"]);
  const NAMES = [];
  for (const f of ["power1", "power2", "power3", "power4", "sine", "expo", "circ", "back"])
    for (const d of ["in", "out", "inOut"]) NAMES.push(`${f}.${d}`);
  const SAMPLES = [0.1, 0.25, 0.5, 0.75, 0.9];

  const round = (v, digits = 3) => {
    const k = 10 ** digits;
    const r = Math.round(v * k) / k;
    return Object.is(r, -0) ? 0 : r;
  };
  const px = (text) => {
    const v = parseFloat(text);
    return Number.isFinite(v) ? v : null;
  };
  const transparent = (c) => !c || c === "transparent" || /rgba\([^)]*,\s*0\)$/.test(c);

  // Any colour Chrome resolves (color(srgb ...), oklab(), a color-mix())
  // as rgb() or rgba(), read back from one painted pixel: the format reads
  // only those, and the 8-bit pixel is what the page shows anyway.
  const paint = document.createElement("canvas").getContext("2d", { willReadFrequently: true });
  const colorCache = new Map();
  function rgb(color) {
    if (colorCache.has(color)) return colorCache.get(color);
    let out = color;
    if (/^rgba?\(/.test(color) && !/\//.test(color)) out = color;
    else {
      paint.clearRect(0, 0, 1, 1);
      paint.fillStyle = "rgba(0, 0, 0, 0)";
      paint.fillStyle = color;
      paint.fillRect(0, 0, 1, 1);
      const [r, g, b, a] = paint.getImageData(0, 0, 1, 1).data;
      out = a === 255 ? `rgb(${r}, ${g}, ${b})` : `rgba(${r}, ${g}, ${b}, ${round(a / 255, 3)})`;
    }
    colorCache.set(color, out);
    return out;
  }
  // Every colour function inside a longer value (a gradient, a filter).
  function rgbIn(text) {
    let out = "";
    let i = 0;
    const re = /(rgba?|color|oklab|oklch|lab|lch|hsla?|hwb|color-mix)\(/g;
    let m;
    while ((m = re.exec(text))) {
      let depth = 0;
      let j = m.index + m[1].length;
      for (; j < text.length; j++) {
        if (text[j] === "(") depth++;
        else if (text[j] === ")" && --depth === 0) break;
      }
      out += text.slice(i, m.index) + rgb(text.slice(m.index, j + 1));
      i = j + 1;
      re.lastIndex = i;
    }
    return out + text.slice(i);
  }
  // A value's items at the top level: "a(b, c), d" is ["a(b, c)", "d"].
  function topLevel(text) {
    const out = [];
    let depth = 0;
    let start = 0;
    for (let i = 0; i < text.length; i++) {
      if (text[i] === "(") depth++;
      else if (text[i] === ")") depth--;
      else if (text[i] === "," && depth === 0) { out.push(text.slice(start, i).trim()); start = i + 1; }
    }
    out.push(text.slice(start).trim());
    return out;
  }
  // Set by __cmExtract to the notes of the composition being read.
  let note = () => {};
  function gradient(text, w = 0, h = 0, path = "?") {
    let g = rgbIn(text);
    // Stops in px, as % of the gradient line or ray (the format reads only
    // %). A linear line's length follows its angle ("to right" is 90deg);
    // a radial ray runs from the centre to the farthest corner, as CSS's
    // default extent, and an ellipse is measured along its horizontal axis.
    const SIDES = { "to top": 0, "to right": 90, "to bottom": 180, "to left": 270, "to top right": 45,
      "to right top": 45, "to bottom right": 135, "to right bottom": 135, "to bottom left": 225,
      "to left bottom": 225, "to top left": 315, "to left top": 315 };
    let line = 0;
    const deg = g.match(/^linear-gradient\((-?[\d.]+)deg/);
    const side = g.match(/^linear-gradient\((to [a-z ]+?),/);
    const angle = deg ? Number(deg[1]) : side && SIDES[side[1]] !== undefined ? SIDES[side[1]] : null;
    if (angle !== null) line = Math.abs(w * Math.sin(angle * Math.PI / 180)) + Math.abs(h * Math.cos(angle * Math.PI / 180));
    else if (/^radial-gradient/.test(g)) {
      const at = g.match(/at (-?[\d.]+)% (-?[\d.]+)%/);
      const cx = at ? Number(at[1]) / 100 * w : w / 2;
      const cy = at ? Number(at[2]) / 100 * h : h / 2;
      const dx = Math.max(cx, w - cx);
      const dy = Math.max(cy, h - cy);
      line = /^radial-gradient\(circle/.test(g) ? Math.hypot(dx, dy) : dx * Math.SQRT2;
    }
    const head = g.match(/^[a-z-]+\([^,]*,/);
    const stops = head ? g.slice(head[0].length) : g;
    if (/-?[\d.]+px/.test(stops)) {
      if (line > 0) g = (head ? head[0] : "") + stops.replace(/(\)\s+)(-?[\d.]+)px/g, (_, a, v) => `${a}${round(v / line * 100, 3)}%`);
      else note(path, "arrêts de dégradé en px lus sans longueur");
    }
    // An explicit radial size the format cannot read: its shape is kept.
    g = g.replace(/^radial-gradient\((?:-?[\d.]+(?:px|%)\s*){1,2}(at\s|,)/, (_, at) => { note(path, "taille de dégradé radial non reprise"); return at === "," ? "radial-gradient(ellipse," : `radial-gradient(ellipse ${at}`; });
    const m = g.match(/^linear-gradient\((.*)\)$/);
    if (m && !/^\s*(-?[\d.]+(deg|turn|rad)|to\s)/.test(m[1])) g = `linear-gradient(180deg, ${m[1]})`;
    // "at center", "at left top": the format reads a centre as two lengths.
    const where = { left: ["0%", null], right: ["100%", null], top: [null, "0%"], bottom: [null, "100%"], center: [null, null] };
    g = g.replace(/\bat ((?:center|left|right|top|bottom)(?: (?:center|left|right|top|bottom))?)(?=,)/, (_, words) => {
      let x = "50%";
      let y = "50%";
      for (const w of words.split(" ")) { if (where[w][0]) x = where[w][0]; if (where[w][1]) y = where[w][1]; }
      return `at ${x} ${y}`;
    });
    return g;
  }
  // A CSS filter in the format's order: drop-shadow(dx dy σ colour), σ
  // half the CSS blur radius.
  function filter(text) {
    if (!text || text === "none") return "none";
    const parts = [];
    const re = /([\w-]+)\(((?:[^()]|\([^()]*\))*)\)/g;
    let m;
    while ((m = re.exec(text))) {
      const [, fn, arg] = m;
      if (fn === "blur") parts.push(`blur(${round(px(arg), 3)}px)`);
      else if (fn === "drop-shadow") {
        const color = (arg.match(/(rgba?|color|oklab|oklch|color-mix)\([^)]*\)|#[0-9a-f]+/i) || ["rgba(0, 0, 0, 0.5)"])[0];
        const nums = arg.replace(color, "").trim().split(/\s+/).map(px).filter((v) => v !== null);
        parts.push(`drop-shadow(${round(nums[0] || 0)}px ${round(nums[1] || 0)}px ${round((nums[2] || 0) / 2, 2)}px ${rgb(color)})`);
      } else return null;
    }
    return parts.length ? parts.join(" ") : null;
  }

  // A seconds value as exact frames: an integer, or "n/d" (MW1).
  const gcd = (a, b) => { a = Math.abs(a); b = Math.abs(b); while (b) [a, b] = [b, a % b]; return a; };
  const framesOf = (seconds, fps) => {
    // A time on the frame grid, or on a simple fraction of it (1/30 s at
    // 30 fps is frame 1, not 1749999/100000), read as that fraction.
    const f = seconds * fps;
    if (Math.abs(f - Math.round(f)) < 1e-6) return Math.round(f);
    let [h0, h1, k0, k1] = [0, 1, 1, 0];
    let x = f;
    for (let i = 0; i < 20; i++) {
      const a = Math.floor(x);
      [h0, h1] = [h1, a * h1 + h0];
      [k0, k1] = [k1, a * k1 + k0];
      if (k1 > 1000) break;
      if (Math.abs(h1 / k1 - f) < 1e-6) return k1 === 1 ? h1 : `${h1}/${k1}`;
      if (x - a < 1e-12) break;
      x = 1 / (x - a);
    }
    // Otherwise the decimal an author wrote, to the microsecond.
    const micro = Math.round(seconds * 1e6);
    let n = micro * fps;
    let d = 1e6;
    const g = gcd(n, d);
    n /= g; d /= g;
    return d === 1 ? n : `${n}/${d}`;
  };
  const frameValue = (f) => typeof f === "number" ? f : (([n, d]) => n / d)(f.split("/").map(Number));

  function easeName(tween) {
    const v = tween.vars || {};
    let name = typeof v.ease === "string" ? v.ease.replace(/\s/g, "") : null;
    if (!name && typeof tween._ease === "function") {
      const sig = (fn) => SAMPLES.map((x) => fn(x));
      const s = sig(tween._ease);
      for (const n of ["none", ...NAMES]) {
        const t = sig(window.gsap.parseEase(n));
        if (t.every((y, k) => Math.abs(y - s[k]) < 1e-9)) { name = n; break; }
      }
    }
    if (!name) return { kind: "bake", name: "fonction" };
    if (name === "none" || name === "linear" || /^power0/.test(name)) return { kind: "named", easing: "linear" };
    const m = name.match(/^(power[1-4]|quad|cubic|quart|quint|strong|sine|expo|circ|back)(\.(in|out|inOut))?(\(([\d.]+)\))?$/);
    if (m) return { kind: "named", easing: name };
    if (/^steps\(1\)$/.test(name)) return { kind: "named", easing: "step-end" };
    return { kind: "bake", name };
  }

  // --- the author's stylesheet: which properties were written, not used ---
  function specificity(selector) {
    const ids = (selector.match(/#[\w-]+/g) || []).length;
    const classes = (selector.match(/(\.[\w-]+|\[[^\]]+\]|:(?!:)[\w-]+)/g) || []).length;
    const tags = (selector.replace(/[#.:\[][^\s>+~]*/g, " ").match(/(^|[\s>+~])[a-zA-Z][\w-]*/g) || []).length;
    return ids * 10000 + classes * 100 + tags;
  }
  const rules = [];
  const collect = (list) => {
    for (const rule of list) {
      if (rule.type === CSSRule.STYLE_RULE) rules.push({ rule, order: rules.length });
      else if (rule.type === CSSRule.MEDIA_RULE && window.matchMedia(rule.media.mediaText).matches) collect(rule.cssRules);
    }
  };
  for (const sheet of document.styleSheets) {
    try { collect(sheet.cssRules); } catch (e) { /* cross-origin sheet */ }
  }
  function specified(el) {
    const found = [];
    for (const { rule, order } of rules) {
      let best = -1;
      for (const sel of rule.selectorText.split(",")) {
        try { if (el.matches(sel.trim())) best = Math.max(best, specificity(sel)); } catch (e) { /* */ }
      }
      if (best >= 0) found.push({ rule, order, best });
    }
    found.sort((a, b) => a.best - b.best || a.order - b.order);
    const out = {};
    for (const { rule } of found) for (const p of rule.style) out[p] = rule.style.getPropertyValue(p).trim();
    for (const p of el.style) out[p] = el.style.getPropertyValue(p).trim();
    return out;
  }
  const written = (spec, name) => spec[name] !== undefined && spec[name] !== "" && spec[name] !== "auto" &&
    spec[name] !== "initial" && spec[name] !== "unset";

  window.__cmExtract = (options) => {
    const notes = [];
    note = (path, what) => notes.push(`${path}: ${what}`);
    const timelines = window.__timelines || {};
    let id = null;
    let best = -1;
    for (const [name, tl] of Object.entries(timelines)) {
      const n = tl.getChildren ? tl.getChildren(true, true, true).length : 0;
      if (n > best) { best = n; id = name; }
    }
    const tl = timelines[id];
    const root = document.querySelector(`[data-composition-id="${id}"]`) ||
      document.querySelector("[data-composition-id]");
    if (!tl || !root) return { error: "no GSAP timeline and composition root" };
    const fps = Number(root.dataset.fps) || 30;
    const width = Number(root.dataset.width) || Math.round(root.getBoundingClientRect().width);
    const height = Number(root.dataset.height) || Math.round(root.getBoundingClientRect().height);
    const seconds = Number(root.dataset.duration) || (tl.repeat() < 0 ? tl.duration() : tl.totalDuration());
    let duration = Math.round(seconds * fps);
    if (Math.abs(duration - seconds * fps) > 1e-6) {
      duration = Math.ceil(seconds * fps);
      note("composition", `durée de ${seconds} s portée à ${duration} images`);
    }
    // Every reading replays from 0 forward, so a zero-duration set is
    // rendered on the way (seeking onto the time the playhead already holds
    // renders nothing, and seeking backwards undoes sets).
    // Going back replays from 0; going forward continues from where the
    // playhead is, which renders the sets on the way just the same.
    let playhead = null;
    let seekErrors = 0;
    const seek = (t) => {
      const back = playhead === null || t < playhead;
      for (const other of Object.values(timelines)) {
        if (!other.seek) continue;
        // A page callback that fails on some frame (a canvas API Chromium
        // lacks) leaves the DOM as GSAP set it: the reading goes on.
        try {
          if (back) other.seek(0, false);
          other.seek(t, false);
        } catch (e) {
          seekErrors++;
        }
      }
      playhead = t;
    };
    seek(0);
    const rootRect = root.getBoundingClientRect();
    // MW4 -- every element a tween moves, known before the tree is read: a
    // text whose words are animated one by one keeps one node per word.
    const animated = new Set();
    const collectTargets = (timeline) => {
      for (const c of timeline.getChildren(false, true, true)) {
        if (c.getChildren) { collectTargets(c); continue; }
        if (c.timeline && c.timeline.getChildren) { collectTargets(c.timeline); continue; }
        for (const t of (c.targets ? c.targets() : [])) if (t instanceof Element) animated.add(t);
      }
    };
    collectTargets(tl);
    // Whether a script drives anything (an onUpdate on a tween or on a
    // timeline), known before the tree too.
    let scripted = false;
    const scan = (t) => {
      if (t.eventCallback && typeof t.eventCallback("onUpdate") === "function") scripted = true;
      for (const c of t.getChildren ? t.getChildren(true, true, true) : []) {
        if (typeof (c.vars || {}).onUpdate === "function") scripted = true;
        if (c.eventCallback && typeof c.eventCallback("onUpdate") === "function") scripted = true;
      }
    };
    for (const t of Object.values(timelines)) scan(t);

    // --- the tree --------------------------------------------------------
    const nodes = new Map(); // element -> node
    const paths = new Map(); // element -> path for notes
    const fontCount = new Map();
    const ids = new Set();
    let count = 0;

    // MW4 -- var(--x) in what the author wrote, resolved on the element:
    // a percentage written through a variable stays a percentage
    // (width: var(--tc-rating-fill) is 100% of the stars, not 91.61 px).
    function resolveVars(v) {
      for (let i = 0; i < 8 && /var\(/.test(v); i++) {
        v = v.replace(/var\(\s*(--[\w-]+)\s*(?:,\s*([^()]*))?\)/g, (_, name, fallback) =>
          (getComputedStyle(currentBoxEl).getPropertyValue(name) || "").trim() || (fallback || "").trim());
      }
      return v.trim();
    }
    let currentBoxEl = null;
    const pendingBorders = new WeakMap(); // a box's style -> its border
    // The border of an element as {widths, colors, styles, radius, w, h},
    // or null: sides of style none or hidden, or transparent, draw nothing.
    function borderOf(el, cs, size) {
      const sides = ["Top", "Right", "Bottom", "Left"];
      const widths = sides.map((side) => {
        const style = cs[`border${side}Style`];
        const width = px(cs[`border${side}Width`]) || 0;
        const color = rgb(cs[`border${side}Color`]);
        return style === "none" || style === "hidden" || transparent(color) ? 0 : width;
      });
      if (!widths.some((v) => v > 0)) return null;
      return { widths, colors: sides.map((side) => rgb(cs[`border${side}Color`])), styles: sides.map((side) => cs[`border${side}Style`]),
        w: size ? size[0] : el.offsetWidth, h: size ? size[1] : el.offsetHeight };
    }
    function addBorder(node, border, path) {
      const { widths, colors, styles, w, h } = border;
      node.children = node.children || [];
      // Children placed absolutely stand inside the border in CSS: the
      // format's box has none, so they move in by its width.
      for (const child of node.children) {
        const cst = child.style || {};
        if (cst.position !== "absolute") continue;
        [["top", 0], ["right", 1], ["bottom", 2], ["left", 3]].forEach(([key, k]) => {
          if (typeof cst[key] === "number") cst[key] = round(cst[key] + widths[k], 3);
        });
      }
      const uniform = widths.every((v) => v === widths[0]) && colors.every((c) => c === colors[0]) && styles.every((v) => v === styles[0]);
      const added = [];
      if (uniform && w > 0 && h > 0) {
        // One stroke round the box, inside its edge: an inside left clear
        // (a ring round a logo), the box's opacity over it like a child's.
        const bw = widths[0];
        const r = Math.max(0, Math.min((node.style.borderRadius || 0), w / 2, h / 2) - bw / 2);
        const x0 = bw / 2, y0 = bw / 2, x1 = w - bw / 2, y1 = h - bw / 2;
        const f = (v) => round(v, 3);
        const d = r > 0
          ? `M${f(x0 + r)} ${f(y0)}H${f(x1 - r)}A${f(r)} ${f(r)} 0 0 1 ${f(x1)} ${f(y0 + r)}V${f(y1 - r)}A${f(r)} ${f(r)} 0 0 1 ${f(x1 - r)} ${f(y1)}` +
            `H${f(x0 + r)}A${f(r)} ${f(r)} 0 0 1 ${f(x0)} ${f(y1 - r)}V${f(y0 + r)}A${f(r)} ${f(r)} 0 0 1 ${f(x0 + r)} ${f(y0)}Z`
          : `M${f(x0)} ${f(y0)}H${f(x1)}V${f(y1)}H${f(x0)}Z`;
        const style = { position: "absolute", left: 0, top: 0, width: "100%", height: "100%", fill: "none", stroke: colors[0], strokeWidth: round(bw, 3) };
        if (styles[0] === "dashed") style.strokeDasharray = `${f(3 * bw)} ${f(3 * bw)}`;
        if (styles[0] === "dotted") { style.strokeDasharray = `0 ${f(2 * bw)}`; style.strokeLinecap = "round"; }
        if (!["solid", "dashed", "dotted"].includes(styles[0])) note(path, `bordure ${styles[0]} dessinée pleine`);
        added.push({ type: "path", d, viewBox: `0 0 ${f(w)} ${f(h)}`, style });
      } else {
        // Sides that differ (an accent bar on one side): a strip each.
        const strips = [
          { top: 0, left: 0, right: 0, height: widths[0] }, { top: 0, right: 0, bottom: 0, width: widths[1] },
          { bottom: 0, left: 0, right: 0, height: widths[2] }, { top: 0, left: 0, bottom: 0, width: widths[3] }];
        strips.forEach((strip, k) => {
          if (!widths[k]) return;
          added.push({ type: "box", style: { position: "absolute", ...strip, backgroundColor: colors[k], flexDirection: "column" } });
        });
        if (node.style.borderRadius) note(path, "bordure inégale sur coins arrondis : bandes droites");
      }
      count += added.length;
      // Over the box's background, under its content.
      node.children.unshift(...added);
    }
    function boxStyle(el, cs, spec, path, isText) {
      const s = {};
      currentBoxEl = el;
      spec = Object.fromEntries(Object.entries(spec).map(([k, v]) => [k, /var\(/.test(v) ? resolveVars(v) : v]));
      const position = cs.position;
      if (position === "absolute" || position === "fixed") {
        s.position = "absolute";
        for (const side of ["top", "right", "bottom", "left"]) {
          if (written(spec, side)) {
            const v = spec[side].endsWith("%") && !spec[side].includes("(") ? spec[side] : px(cs[side]);
            if (v !== null) s[side] = typeof v === "number" ? round(v) : v;
          }
        }
        if (written(spec, "inset") && s.top === undefined) {
          for (const side of ["top", "right", "bottom", "left"]) { const v = px(cs[side]); if (v !== null) s[side] = round(v); }
        }
      }
      for (const dim of ["width", "height", "min-width", "min-height", "max-width", "max-height"]) {
        if (!written(spec, dim) || spec[dim] === "none") continue;
        const key = dim.replace(/-(\w)/g, (_, c) => c.toUpperCase());
        const v = spec[dim];
        if (/^-?[\d.]+%$/.test(v)) s[key] = v;
        else if (["fit-content", "max-content", "min-content"].includes(v)) continue;
        else { const n = px(cs[key]); if (n !== null) s[key] = round(n); }
      }
      // MW4 -- one side written and an aspect-ratio: the other is the
      // ratio's, as Chrome laid it out (an avatar's circle).
      if (written(spec, "aspect-ratio") && (s.width === undefined) !== (s.height === undefined)) {
        if (s.width === undefined) s.width = round(px(cs.width), 2);
        else s.height = round(px(cs.height), 2);
      }
      if (!isText) {
        const display = cs.display;
        if (display === "flex" || display === "inline-flex") {
          s.flexDirection = cs.flexDirection;
          if (cs.flexWrap !== "nowrap") s.flexWrap = cs.flexWrap;
        } else if (display === "grid" || display === "inline-grid") {
          // MW4 -- a grid of one cell (place-items: center) is a flexbox
          // column, so a longer prop still centres; only a grid of several
          // tracks keeps Chrome's positions (build), at its used size.
          s.flexDirection = "column";
          if (gridKind(el, cs) === "multi") {
            if (s.width === undefined) s.width = round(px(cs.width), 2);
            if (s.height === undefined) s.height = round(px(cs.height), 2);
            note(path, "grille CSS : enfants placés aux positions calculées par Chrome");
          }
        } else {
          s.flexDirection = "column";
        }
        const justify = { normal: "flex-start", start: "flex-start", end: "flex-end", left: "flex-start", right: "flex-end" }[cs.justifyContent] || cs.justifyContent;
        if (justify !== "flex-start" && ["center", "flex-end", "space-between", "space-around", "space-evenly"].includes(justify)) s.justifyContent = justify;
        const align = { normal: "stretch", start: "flex-start", end: "flex-end", "self-start": "flex-start", "self-end": "flex-end" }[cs.alignItems] || cs.alignItems;
        if (align !== "stretch" && ["flex-start", "center", "flex-end", "baseline"].includes(align)) s.alignItems = align;
        for (const [css, key] of [["rowGap", "rowGap"], ["columnGap", "columnGap"]]) {
          const v = px(cs[css]);
          if (v) s[key] = round(v);
        }
        if (s.rowGap !== undefined && s.rowGap === s.columnGap) { s.gap = s.rowGap; delete s.rowGap; delete s.columnGap; }
        if ((display === "grid" || display === "inline-grid") && gridKind(el, cs) === "single") {
          // In a grid, align-* is vertical and justify-* horizontal: the
          // other way round from a flexbox column.
          delete s.justifyContent;
          delete s.alignItems;
          delete s.gap;
          delete s.rowGap;
          delete s.columnGap;
          Object.assign(s, gridAxis(cs));
        }
      }
      const alignSelf = { start: "flex-start", end: "flex-end" }[cs.alignSelf] || cs.alignSelf;
      if (["flex-start", "center", "flex-end", "stretch", "baseline"].includes(alignSelf) && written(spec, "align-self")) s.alignSelf = alignSelf;
      if (px(cs.flexGrow)) s.flexGrow = px(cs.flexGrow);
      if (px(cs.flexShrink) !== 1) s.flexShrink = px(cs.flexShrink);
      if (cs.whiteSpace === "nowrap" || cs.whiteSpace === "pre") s.flexShrink = 0;
      for (const side of ["Top", "Right", "Bottom", "Left"]) {
        const p = px(cs["padding" + side]);
        if (p) s["padding" + side] = round(p);
        const m = px(cs["margin" + side]);
        if (m) s["margin" + side] = round(m);
      }
      if (!transparent(rgb(cs.backgroundColor))) s.backgroundColor = rgb(cs.backgroundColor);
      if (cs.backgroundImage && cs.backgroundImage !== "none") {
        // MW4 -- background-size and -repeat hold one value per layer:
        // only a layer sized smaller than the box and repeated is a
        // pattern; it is left out on its own, the others kept.
        const all = topLevel(cs.backgroundImage);
        const sizes = topLevel(cs.backgroundSize || "auto");
        const repeats = topLevel(cs.backgroundRepeat || "repeat");
        const layers = [];
        all.forEach((layer, k) => {
          if (layer === "none") return;
          const size = sizes[k % sizes.length];
          const repeat = repeats[k % repeats.length];
          if (!/^(auto|auto auto|100% 100%|100%|cover|contain)$/.test(size) && !/^no-repeat( no-repeat)?$/.test(repeat)) {
            note(path, `motif répété (couche ${k + 1}, background-size ${size}) non repris`);
            return;
          }
          layers.push(layer);
        });
        if (layers.length && /^(linear|radial)-gradient/.test(layers[0])) {
          s.backgroundImage = gradient(layers[0], el.offsetWidth, el.offsetHeight, path);
          if (layers.length > 1) note(path, "plusieurs fonds : seul le premier est repris");
        } else if (layers.length) {
          note(path, "image de fond non reprise");
        }
      }
      const radius = cs.borderTopLeftRadius;
      if (radius && radius !== "0px") {
        const rect = el.getBoundingClientRect();
        const r = radius.endsWith("%") ? parseFloat(radius) / 100 * Math.min(el.offsetWidth || rect.width, el.offsetHeight || rect.height) : px(radius);
        if (r) s.borderRadius = round(Math.min(r, Math.min(el.offsetWidth, el.offsetHeight) / 2 || r));
      }
      if (cs.overflow === "hidden" || cs.overflow === "clip") s.overflow = "hidden";
      // MW4 -- a border, drawn by a path or strips once the node exists
      // (addBorder). The format has none: its width goes into the
      // padding, so the content stays where the border pushed it.
      const border = borderOf(el, cs);
      if (border && !isText) {
        ["Top", "Right", "Bottom", "Left"].forEach((side, k) => {
          if (border.widths[k]) s["padding" + side] = round((s["padding" + side] || 0) + border.widths[k], 3);
        });
        pendingBorders.set(s, border);
      }
      if (cs.boxShadow && cs.boxShadow !== "none") {
        const m = cs.boxShadow.match(/^(rgba?\([^)]*\))\s+(-?[\d.]+)px\s+(-?[\d.]+)px\s+([\d.]+)px/);
        // CSS blurs a shadow over a radius of about 2σ; the format takes σ.
        if (m && !cs.boxShadow.includes("inset")) s.filter = `drop-shadow(${m[2]}px ${m[3]}px ${round(Number(m[4]) / 2, 2)}px ${rgb(m[1])})`;
        else note(path, "ombre non reprise");
      }
      if (cs.mixBlendMode && cs.mixBlendMode !== "normal") s.mixBlendMode = cs.mixBlendMode;
      const origin = cs.transformOrigin.split(" ").map(parseFloat);
      const rect = { w: el.offsetWidth, h: el.offsetHeight };
      if (rect.w && rect.h && (Math.abs(origin[0] - rect.w / 2) > 0.5 || Math.abs(origin[1] - rect.h / 2) > 0.5)) {
        nodes.__origins = (nodes.__origins || 0) + 1;
        note(path, `transform-origin ${cs.transformOrigin} : le format tourne et met à l'échelle autour du centre (MW5)`);
      }
      return s;
    }

    // The web's generic and system families, and brand faces no one can
    // install, as faces every Mac has: the engine refuses a missing font.
    const SYSTEM = { "-apple-system": "Helvetica Neue", "system-ui": "Helvetica Neue",
      "ui-sans-serif": "Helvetica Neue", "BlinkMacSystemFont": "Helvetica Neue", "sans-serif": "Helvetica Neue",
      "ui-monospace": "Menlo", "monospace": "Menlo", "SFMono-Regular": "Menlo", "SF Mono": "Menlo",
      "ui-serif": "Times New Roman", "serif": "Times New Roman", "Apple Color Emoji": "Apple Color Emoji" };
    const BRAND = /^(ClaudeSans|Avatar\w*|Anthropic\w*|HeyGen\w*)$/;
    // MW4 -- whether the face a style names is there to measure with. Not
    // document.fonts.check(), which answers true for any face it does not
    // manage: a sample is measured in the face and in two fallbacks, and a
    // missing face measures as its fallback in both.
    const fontsSeen = new Map();
    function fontAvailable(family) {
      if (fontsSeen.has(family)) return fontsSeen.get(family);
      const sample = "Hamburgefonstiv 0123456789 WMwm";
      const measure = (font) => { paint.font = font; return paint.measureText(sample).width; };
      const ok = ["monospace", "serif"].some((fallback) =>
        measure(`72px "${family}", ${fallback}`) !== measure(`72px ${fallback}`));
      fontsSeen.set(family, ok);
      if (!ok) note("police", `police absente à l'import : ${family}, mesures faites dans une police de repli`);
      return ok;
    }
    function textStyle(cs, s) {
      let family = cs.fontFamily.split(",")[0].trim().replace(/^["']|["']$/g, "");
      if (!SYSTEM[family] && !BRAND.test(family)) fontAvailable(family);
      if (SYSTEM[family]) family = SYSTEM[family];
      else if (BRAND.test(family)) { note("police", `${family} (police de marque) remplacée par Helvetica Neue`); family = "Helvetica Neue"; }
      s.fontFamily = family;
      fontCount.set(family, (fontCount.get(family) || 0) + 1);
      s.fontSize = round(px(cs.fontSize));
      const weight = Number(cs.fontWeight);
      if (weight && weight !== 400) s.fontWeight = weight;
      if (cs.fontStyle === "italic") s.fontStyle = "italic";
      if (cs.lineHeight !== "normal") s.lineHeight = `${round(px(cs.lineHeight))}px`;
      const spacing = px(cs.letterSpacing);
      if (spacing) s.letterSpacing = round(spacing);
      s.color = rgb(cs.color);
      if (cs.textTransform !== "none") s.textTransform = cs.textTransform;
      const align = { start: "left", end: "right", justify: "left", "-webkit-left": "left" }[cs.textAlign] || cs.textAlign;
      if (align === "center" || align === "right") s.textAlign = align;
      if (cs.textShadow && cs.textShadow !== "none" && !s.filter) {
        const shadows = topLevel(cs.textShadow);
        const m = shadows[0].match(/^(.*\))\s+(-?[\d.]+)px\s+(-?[\d.]+)px(?:\s+([\d.]+)px)?/);
        if (m) s.filter = `drop-shadow(${round(m[2])}px ${round(m[3])}px ${round(Number(m[4] || 0) / 2, 2)}px ${rgb(m[1])})`;
        if (shadows.length > 1 || !m) note("text-shadow", "ombre de texte multiple : seule la première est reprise");
      }
    }

    const ownHidden = (el) => getComputedStyle(el).visibility === "hidden" &&
      (!el.parentElement || getComputedStyle(el.parentElement).visibility !== "hidden");
    // MW4 -- a grid of one cell, or of several tracks. One cell: a single
    // computed track each way, or every in-flow child on the same area.
    function gridKind(el, cs) {
      const tracks = (v) => (v || "none").trim() === "none" ? 0 : topLevel(v.replace(/\s+/g, ",").replace(/\(,/g, "(").replace(/,\)/g, ")")).length;
      const flow = [...el.children].filter((c) => {
        const ccs = getComputedStyle(c);
        return ccs.display !== "none" && ccs.position !== "absolute" && ccs.position !== "fixed" && !["script", "style", "template"].includes(c.tagName.toLowerCase());
      });
      if (tracks(cs.gridTemplateColumns) <= 1 && tracks(cs.gridTemplateRows) <= 1) return "single";
      const areas = new Set(flow.map((c) => { const ccs = getComputedStyle(c); return `${ccs.gridRowStart}/${ccs.gridColumnStart}`; }));
      if (flow.length > 1 && areas.size === 1 && ![...areas][0].includes("auto")) return "single";
      return "multi";
    }
    const VERTICAL = { center: "center", start: "flex-start", "flex-start": "flex-start", "self-start": "flex-start",
      end: "flex-end", "flex-end": "flex-end", "self-end": "flex-end", "space-between": "space-between",
      "space-around": "space-around", "space-evenly": "space-evenly" };
    const HORIZONTAL = { center: "center", start: "flex-start", "flex-start": "flex-start", "self-start": "flex-start",
      left: "flex-start", end: "flex-end", "flex-end": "flex-end", "self-end": "flex-end", right: "flex-end",
      stretch: "stretch", normal: "stretch", legacy: "stretch" };
    // A one-cell grid's alignment as a flexbox column: align-content then
    // align-items place vertically, justify-items horizontally.
    function gridAxis(cs, child) {
      const ccs = child ? getComputedStyle(child) : null;
      const vertical = (ccs && VERTICAL[ccs.alignSelf]) || VERTICAL[cs.alignContent] || VERTICAL[cs.alignItems] || "flex-start";
      const horizontal = (ccs && ccs.justifySelf !== "auto" && HORIZONTAL[ccs.justifySelf]) ||
        HORIZONTAL[cs.justifyItems.replace("legacy", "").trim() || "normal"] || "stretch";
      const out = {};
      if (vertical !== "flex-start") out.justifyContent = vertical;
      if (horizontal !== "stretch") out.alignItems = horizontal;
      return out;
    }
    // Boxes as Chrome lays them out, transforms aside and in fractional
    // pixels (offsetWidth rounds: "HYPERFRAMES" at 1349.2 px in a 1349 px
    // box breaks in the engine).
    const untransform = document.createElement("style");
    untransform.textContent = "* { transform: none !important; translate: none !important; rotate: none !important; scale: none !important; }";
    function layoutRects(elements) {
      document.head.appendChild(untransform);
      const rects = elements.map((e) => e.getBoundingClientRect());
      untransform.remove();
      return rects;
    }
    // A grid's children. One cell: flexbox, each child aligned by the grid
    // (several children sharing the cell each get a full-size layer). Several
    // tracks: Chrome's positions; a text keeps its place but never a width,
    // so the engine measures it.
    function layoutGridChildren(el, cs, node, pairs) {
      if (gridKind(el, cs) === "single") {
        if (pairs.length > 1) {
          node.children = pairs.map(({ el: c, node: child }) => {
            if (child.style.position === "absolute") return child;
            return { type: "box", style: { position: "absolute", top: 0, right: 0, bottom: 0, left: 0, flexDirection: "column",
              ...gridAxis(cs, c) }, children: [child] };
          });
        } else if (pairs.length === 1) {
          const { el: c, node: child } = pairs[0];
          const own = gridAxis(cs, c);
          if (own.justifyContent) node.style.justifyContent = own.justifyContent;
          if (own.alignItems && own.alignItems !== node.style.alignItems) child.style.alignSelf = own.alignItems;
        }
        return;
      }
      const flow = pairs.filter(({ el: c, node: child }) => c instanceof HTMLElement && child.style.position !== "absolute");
      const [outer, ...inner] = layoutRects([el, ...flow.map((p) => p.el)]);
      const borderLeft = px(cs.borderLeftWidth) || 0;
      const borderTop = px(cs.borderTopWidth) || 0;
      flow.forEach(({ node: child }, k) => {
        const r = inner[k];
        const placed = Object.assign(child.style, { position: "absolute", left: round(r.left - outer.left - borderLeft, 2),
          top: round(r.top - outer.top - borderTop, 2) });
        for (const k2 of ["right", "bottom", "marginTop", "marginRight", "marginBottom", "marginLeft"]) delete placed[k2];
        // MW4 -- a box nothing paints and nothing centres takes its
        // content's size: at Chrome's width, 0.04 px short in the engine,
        // a row of text breaks ("5 / 5" of testimonial-card).
        const st = child.style;
        const shows = st.backgroundColor || st.backgroundImage || st.borderRadius || st.overflow || st.filter;
        const across = st.flexDirection === "row" ? st.justifyContent : st.alignItems;
        const centres = ["center", "flex-end", "space-between", "space-around", "space-evenly"].includes(across) ||
          child.children?.some((g) => g.style?.textAlign);
        if (child.type !== "text") {
          if (placed.width === undefined && (shows || centres || !child.children?.length)) placed.width = round(r.width, 2);
          if (placed.height === undefined) placed.height = round(r.height, 2);
        }
      });
    }
    // Where an element sits in the page, transforms aside.
    function layoutBox(el) {
      let x = 0;
      let y = 0;
      for (let e = el; e; e = e.offsetParent) { x += e.offsetLeft; y += e.offsetTop; }
      return { x, y, w: el.offsetWidth, h: el.offsetHeight };
    }
    // The translation of the computed transform, in px whatever the unit
    // the author wrote (GSAP reads "2cqh" as 2).
    function translation(el) {
      const t = getComputedStyle(el).transform;
      if (!t || t === "none") return [0, 0];
      const m = new DOMMatrix(t);
      return [m.m41, m.m42];
    }

    function staticTransform(el, s) {
      const g = window.gsap;
      const [tx, ty] = translation(el);
      const x = round(tx, 2);
      const y = round(ty, 2);
      if (x) s.translateX = x;
      if (y) s.translateY = y;
      const sx = round(g.getProperty(el, "scaleX"), 4);
      const sy = round(g.getProperty(el, "scaleY"), 4);
      if (sx === sy && sx !== 1) s.scale = sx;
      else if (sx !== sy) { s.scaleX = sx; s.scaleY = sy; }
      const r = round(g.getProperty(el, "rotation"), 3);
      if (r) s.rotate = r;
    }

    // An SVG shape as path data, so a line or a circle is drawn too.
    function shapePath(p) {
      const n = (a) => Number(p.getAttribute(a) || 0);
      switch (p.tagName.toLowerCase()) {
        case "path": return p.getAttribute("d");
        case "line": return `M${n("x1")} ${n("y1")}L${n("x2")} ${n("y2")}`;
        case "rect": return `M${n("x")} ${n("y")}h${n("width")}v${n("height")}h${-n("width")}Z`;
        case "circle": case "ellipse": {
          // From three o'clock, clockwise, as SVG draws a circle: a dash
          // or a trim then starts where the page's does.
          const rx = n("r") || n("rx");
          const ry = n("r") || n("ry");
          const cx = n("cx");
          const cy = n("cy");
          return `M${cx + rx} ${cy}A${rx} ${ry} 0 1 1 ${cx - rx} ${cy}A${rx} ${ry} 0 1 1 ${cx + rx} ${cy}Z`;
        }
        case "polygon": case "polyline": {
          const pts = (p.getAttribute("points") || "").trim().split(/[\s,]+/).map(Number);
          if (pts.length < 4) return null;
          let d = `M${pts[0]} ${pts[1]}`;
          for (let i = 2; i + 1 < pts.length; i += 2) d += `L${pts[i]} ${pts[i + 1]}`;
          return p.tagName.toLowerCase() === "polygon" ? d + "Z" : d;
        }
      }
      return null;
    }

    // MW4 -- path data with a transform baked in: absolute commands, arcs
    // as cubics, every point through the matrix (a b c d e f, as SVG).
    function bakePath(d, m) {
      const tokens = d.match(/[a-df-z]|[-+]?(?:\d*\.\d+|\d+\.?)(?:e[-+]?\d+)?/gi) || [];
      let i = 0;
      const num = () => Number(tokens[i++]);
      const isNum = () => i < tokens.length && !/^[a-df-z]$/i.test(tokens[i]);
      const out = [];
      const P = (x, y) => [m.a * x + m.c * y + m.e, m.b * x + m.d * y + m.f];
      const put = (cmd, ...pts) => out.push(cmd + pts.map((q) => P(q[0], q[1]).map((v) => round(v, 3)).join(" ")).join(" "));
      let x = 0, y = 0, sx = 0, sy = 0, cx = null, cy = null, qx = null, qy = null, cmd = "";
      const arc = (rx, ry, phi, large, sweep, x2, y2) => {
        // Endpoint to centre (SVG implementation notes F.6.5), then one
        // cubic per quarter turn at most.
        if (!rx || !ry) { put("L", [x2, y2]); return; }
        rx = Math.abs(rx); ry = Math.abs(ry);
        const t = phi * Math.PI / 180;
        const cos = Math.cos(t), sin = Math.sin(t);
        const dx = (x - x2) / 2, dy = (y - y2) / 2;
        const x1p = cos * dx + sin * dy, y1p = -sin * dx + cos * dy;
        const lambda = x1p * x1p / (rx * rx) + y1p * y1p / (ry * ry);
        if (lambda > 1) { rx *= Math.sqrt(lambda); ry *= Math.sqrt(lambda); }
        const num_ = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p;
        const k = (large === sweep ? -1 : 1) * Math.sqrt(Math.max(0, num_ / (rx * rx * y1p * y1p + ry * ry * x1p * x1p)));
        const cxp = k * rx * y1p / ry, cyp = -k * ry * x1p / rx;
        const ccx = cos * cxp - sin * cyp + (x + x2) / 2, ccy = sin * cxp + cos * cyp + (y + y2) / 2;
        const ang = (ux, uy, vx, vy) => Math.atan2(ux * vy - uy * vx, ux * vx + uy * vy);
        let th1 = ang(1, 0, (x1p - cxp) / rx, (y1p - cyp) / ry);
        let dth = ang((x1p - cxp) / rx, (y1p - cyp) / ry, (-x1p - cxp) / rx, (-y1p - cyp) / ry);
        if (!sweep && dth > 0) dth -= 2 * Math.PI;
        if (sweep && dth < 0) dth += 2 * Math.PI;
        const n = Math.max(1, Math.ceil(Math.abs(dth) / (Math.PI / 2) - 1e-9));
        const step = dth / n;
        const h = 4 / 3 * Math.tan(step / 4);
        const at = (a) => [ccx + rx * Math.cos(a) * cos - ry * Math.sin(a) * sin, ccy + rx * Math.cos(a) * sin + ry * Math.sin(a) * cos];
        const tan = (a) => [-rx * Math.sin(a) * cos - ry * Math.cos(a) * sin, -rx * Math.sin(a) * sin + ry * Math.cos(a) * cos];
        for (let s = 0; s < n; s++) {
          const a0 = th1 + s * step, a1 = a0 + step;
          const [p0, p1, t0, t1] = [at(a0), at(a1), tan(a0), tan(a1)];
          put("C", [p0[0] + h * t0[0], p0[1] + h * t0[1]], [p1[0] - h * t1[0], p1[1] - h * t1[1]], s === n - 1 ? [x2, y2] : p1);
        }
      };
      while (i < tokens.length) {
        if (!isNum()) cmd = tokens[i++];
        const rel = cmd === cmd.toLowerCase();
        const ox = rel ? x : 0, oy = rel ? y : 0;
        const C = cmd.toUpperCase();
        if (C === "Z") {
          out.push("Z"); x = sx; y = sy; cx = qx = null;
          // Numbers right after a Z have no command to read them.
          if (isNum()) return null;
          continue;
        }
        if (C === "M") { x = ox + num(); y = oy + num(); sx = x; sy = y; put("M", [x, y]); cmd = rel ? "l" : "L"; cx = qx = null; continue; }
        if (C === "L") { x = ox + num(); y = oy + num(); put("L", [x, y]); cx = qx = null; continue; }
        if (C === "H") { x = ox + num(); put("L", [x, y]); cx = qx = null; continue; }
        if (C === "V") { y = (rel ? y : 0) + num(); put("L", [x, y]); cx = qx = null; continue; }
        if (C === "C" || C === "S") {
          let x1, y1;
          if (C === "C") { x1 = ox + num(); y1 = oy + num(); }
          else { x1 = cx === null ? x : 2 * x - cx; y1 = cy === null ? y : 2 * y - cy; }
          const x2 = ox + num(), y2 = oy + num(), x3 = ox + num(), y3 = oy + num();
          put("C", [x1, y1], [x2, y2], [x3, y3]);
          cx = x2; cy = y2; qx = null; x = x3; y = y3; continue;
        }
        if (C === "Q" || C === "T") {
          let x1, y1;
          if (C === "Q") { x1 = ox + num(); y1 = oy + num(); }
          else { x1 = qx === null ? x : 2 * x - qx; y1 = qy === null ? y : 2 * y - qy; }
          const x2 = ox + num(), y2 = oy + num();
          put("Q", [x1, y1], [x2, y2]);
          qx = x1; qy = y1; cx = null; x = x2; y = y2; continue;
        }
        if (C === "A") {
          const rx = num(), ry = num(), phi = num(), large = num(), sweep = num();
          const x2 = ox + num(), y2 = oy + num();
          arc(rx, ry, phi, large ? 1 : 0, sweep ? 1 : 0, x2, y2);
          x = x2; y = y2; cx = qx = null; continue;
        }
        return null;
      }
      return out.join(" ");
    }
    // The transform attributes between a shape and its svg, as one matrix.
    function shapeMatrix(p, svg) {
      let m = new DOMMatrix();
      for (let e = p; e && e !== svg; e = e.parentNode) {
        const list = e.transform && e.transform.baseVal;
        const own = list && list.numberOfItems ? list.consolidate() : null;
        if (own) m = new DOMMatrix([own.matrix.a, own.matrix.b, own.matrix.c, own.matrix.d, own.matrix.e, own.matrix.f]).multiply(m);
      }
      return m.isIdentity ? null : m;
    }

    // MW4 -- a shape's dash in its user units (a pathLength attribute
    // rescales the lengths CSS gives), and whether it is a draw-on: one dash
    // and one gap each at least the whole length, so a single stretch of
    // the path shows, which is a trim in the format.
    const totalLengths = new WeakMap();
    function dashOf(p) {
      const cs = getComputedStyle(p);
      // Measured once per geometry: reads come at every frame.
      const key = `${p.getAttribute("d")}|${p.getAttribute("points")}|${p.getAttribute("r")}|${p.getAttribute("width")}|${p.getAttribute("x2")}`;
      let total = 0;
      const known = totalLengths.get(p);
      if (known && known.key === key) total = known.total;
      else {
        try { total = p.getTotalLength(); } catch (e) { /* not a geometry element */ }
        totalLengths.set(p, { key, total });
      }
      const declared = Number(p.getAttribute("pathLength"));
      const unit = declared > 0 && total ? total / declared : 1;
      const list = !cs.strokeDasharray || cs.strokeDasharray === "none" ? [] :
        cs.strokeDasharray.split(/[\s,]+/).map(px).filter((v) => v !== null).map((v) => v * unit);
      const offset = (px(cs.strokeDashoffset) || 0) * unit;
      const dash = list[0];
      const gap = list.length > 1 ? list[1] : list[0];
      const drawOn = total > 0 && list.length > 0 && list.length <= 2 && dash >= total * 0.98 && gap >= total * 0.98;
      return { total, list, offset, drawOn };
    }
    // The drawn stretch of a draw-on as [trimStart, trimEnd]: the dash that
    // starts at -offset, or the one a period before or after it.
    function trimOf(p, length) {
      const d = dashOf(p);
      // A sub-path of a split shape (MW4): its own length, the same dash.
      if (length !== undefined) {
        d.total = length;
        d.drawOn = d.list.length > 0 && d.list.length <= 2 && d.list[0] >= length * 0.98 &&
          (d.list.length > 1 ? d.list[1] : d.list[0]) >= length * 0.98;
      }
      if (!d.list.length || !d.total) return [0, 1];
      if (!d.drawOn) return null;
      const dash = d.list[0];
      const period = dash + (d.list.length > 1 ? d.list[1] : d.list[0]);
      let best = [0, 0];
      for (const k of [-1, 0, 1]) {
        const a = Math.max(0, -d.offset + k * period);
        const b = Math.min(d.total, -d.offset + k * period + dash);
        if (b - a > best[1] - best[0]) best = [a, b];
      }
      // Less than half a pixel drawn is nothing: a remainder of rounding
      // between getTotalLength and CSS, which a round cap shows as a dot.
      if (best[1] - best[0] < 0.5) return [0, 0];
      return [round(best[0] / d.total, 5), round(best[1] / d.total, 5)];
    }
    // A shape's dash or trim as the format writes it.
    function dashStyle(p, s) {
      const d = dashOf(p);
      if (!d.list.length) return;
      if (d.drawOn) {
        const [a, b] = trimOf(p);
        if (a) s.trimStart = a;
        if (b !== 1) s.trimEnd = b;
      } else {
        s.strokeDasharray = d.list.map((v) => round(v, 3)).join(" ");
        if (d.offset) s.strokeDashoffset = round(d.offset, 3);
      }
    }

    // MW4 -- a box that wraps a text (a text with decorations, a text
    // centred in a sized box) -> that text; elements drawn as one text with
    // ranges -> their path, for the animations their spans lose.
    const wrappedText = new Map();
    // MW4 -- an SVG group a tween moves -> how its box maps the group's
    // user space: {svg, k, o: [x, y], c: [x, y]} (viewBox fit, box centre).
    const svgGroups = new Map();
    const forcedBake = new Set(); // nodes whose transform is read frame by frame
    // The matrix of a group and everything above it up to its svg, in user
    // units, as the box's own transform: scale and rotation about the box
    // centre, the origin made good by the translation (as for MW5).
    function groupTransform(g) {
      const info = svgGroups.get(g);
      let m = new DOMMatrix();
      for (let e = g; e && e !== info.svg; e = e.parentNode) {
        const list = e.transform && e.transform.baseVal;
        const own = list && list.numberOfItems ? list.consolidate() : null;
        if (own) m = new DOMMatrix([own.matrix.a, own.matrix.b, own.matrix.c, own.matrix.d, own.matrix.e, own.matrix.f]).multiply(m);
      }
      const { k, o, c } = info;
      const sx = Math.hypot(m.a, m.b);
      const sy = sx ? (m.a * m.d - m.b * m.c) / sx : 0;
      // T = (I - A)(o - c) + k b
      const ux = o[0] - c[0], uy = o[1] - c[1];
      const tx = ux - (m.a * ux + m.c * uy) + k * m.e;
      const ty = uy - (m.b * ux + m.d * uy) + k * m.f;
      return { translateX: round(tx, 3), translateY: round(ty, 3), rotate: round(Math.atan2(m.b, m.a) * 180 / Math.PI, 4),
        scaleX: round(sx, 5), scaleY: round(sy, 5), skewed: Math.abs(m.a * m.c + m.b * m.d) > 1e-3 * (sx * sx + 1e-9) };
    }
    const GROUP_TRANSFORM = ["translateX", "translateY", "rotate", "scaleX", "scaleY"];
    const mixedTexts = new Map();
    const SKIP = ["script", "style", "template", "link", "meta", "noscript"];
    const positioned = (ccs) => ccs.position === "absolute" || ccs.position === "fixed";
    // An element whose content is one text in one inline flow: its own
    // text and inline children, around absolute or SVG decorations (a
    // marker under a word, a highlighter behind it). null when a block,
    // an image or a padded inline box sits in the flow.
    function inlineFlow(el) {
      const decos = [];
      const skipped = [];
      let ok = true;
      const walk = (e) => {
        for (const c of e.children) {
          const tag = c.tagName.toLowerCase();
          if (SKIP.includes(tag) || tag === "br") { skipped.push(c); continue; }
          const ccs = getComputedStyle(c);
          if (ccs.display === "none") { skipped.push(c); continue; }
          if (positioned(ccs) || tag === "svg") { decos.push(c); continue; }
          if (!ccs.display.startsWith("inline") || ["img", "video", "canvas", "iframe"].includes(tag)) { ok = false; return; }
          // An inline box with padding, a border or a size is a chip, not
          // a run of text: its box would be lost.
          const boxy = ["Top", "Right", "Bottom", "Left"].some((side) => (px(ccs["padding" + side]) || 0) > 1 || (px(ccs[`border${side}Width`]) || 0) > 0) ||
            (ccs.backgroundImage && ccs.backgroundImage !== "none");
          if (boxy) { ok = false; return; }
          walk(c);
        }
      };
      walk(el);
      return ok ? { decos, skipped } : null;
    }
    // The text of an inline flow, white space collapsed as CSS does, each
    // character with the element it is written in.
    function flowText(el, flow) {
      const chars = [];
      const out = (n) => flow.decos.some((d) => d.contains(n)) || flow.skipped.some((d) => d.contains(n));
      const walker = document.createTreeWalker(el, NodeFilter.SHOW_TEXT, { acceptNode: (n) => out(n) ? NodeFilter.FILTER_REJECT : NodeFilter.FILTER_ACCEPT });
      for (let n = walker.nextNode(); n; n = walker.nextNode()) {
        const data = n.data;
        for (let i = 0; i < data.length; i++) {
          const space = /[ \t\n\r\f]/.test(data[i]);
          if (space && (!chars.length || chars[chars.length - 1].ch === " ")) continue;
          chars.push({ ch: space ? " " : data[i], owner: n.parentElement });
        }
      }
      while (chars.length && chars[chars.length - 1].ch === " ") chars.pop();
      return chars;
    }
    // What a run of the text looks like against the text itself: the three
    // things a range can say (colour, background, opacity), and what it
    // cannot (another face, size, weight or slant, a transform).
    function runStyle(owner, el, base, lost) {
      const ocs = getComputedStyle(owner);
      const st = {};
      const color = rgb(ocs.color);
      if (color !== base.color) st.color = color;
      let bg = null;
      let opacity = 1;
      for (let e = owner; e && e !== el; e = e.parentElement) {
        const ecs = getComputedStyle(e);
        if (!bg && !transparent(rgb(ecs.backgroundColor))) bg = rgb(ecs.backgroundColor);
        // visibility is inherited: only a run that hides itself against its
        // parent is transparent. A group shown later (a caption line) hides
        // its runs through its own node, not through their ranges.
        const hides = ecs.visibility === "hidden" && e.parentElement && getComputedStyle(e.parentElement).visibility !== "hidden";
        opacity *= hides ? 0 : px(ecs.opacity);
        if (ecs.transform && ecs.transform !== "none") lost.add("transformation");
      }
      if (bg) st.backgroundColor = bg;
      if (opacity !== 1) st.opacity = round(opacity, 4);
      if (ocs.fontFamily !== base.cs.fontFamily) lost.add("police");
      if (ocs.fontSize !== base.cs.fontSize) lost.add("taille");
      if (ocs.fontWeight !== base.cs.fontWeight) lost.add("graisse");
      if (ocs.fontStyle !== base.cs.fontStyle) lost.add("italique");
      if (ocs.textDecorationLine !== base.cs.textDecorationLine) lost.add("soulignement");
      return st;
    }
    // The text's ranges: by word when every word is of one style, by
    // character otherwise; units counted as the engine does (a word is a
    // run without space, a character a grapheme that is not a space).
    function rangesOf(chars, el, cs, path) {
      const lost = new Set();
      const base = { color: rgb(cs.color), cs };
      const styles = new Map();
      const keyOf = (owner) => {
        if (!styles.has(owner)) styles.set(owner, owner === el ? {} : runStyle(owner, el, base, lost));
        return JSON.stringify(styles.get(owner));
      };
      const text = chars.map((c) => c.ch).join("");
      const units = []; // {word, key}
      let word = -1;
      let previousSpace = true;
      for (const { segment, index } of new Intl.Segmenter(undefined, { granularity: "grapheme" }).segment(text)) {
        const space = /^\s+$/.test(segment);
        if (!space && previousSpace) word++;
        previousSpace = space;
        if (!space) units.push({ word, key: keyOf(chars[index].owner) });
      }
      if (lost.size) note(path, `texte à styles mêlés : ${[...lost].join(", ")} des passages non repris`);
      const words = [];
      for (const u of units) {
        if (!words[u.word]) words[u.word] = new Set();
        words[u.word].add(u.key);
      }
      const byWord = words.every((keys) => keys.size === 1);
      const sequence = byWord ? words.map((keys) => [...keys][0]) : units.map((u) => u.key);
      const ranges = [];
      sequence.forEach((key, k) => {
        if (key === "{}") return;
        const last = ranges[ranges.length - 1];
        if (last && last.key === key && last.start + last.count === k) { last.count++; return; }
        ranges.push({ key, start: k, count: 1 });
      });
      return ranges.map(({ key, start, count }) => ({ unit: byWord ? "word" : "char", start, count, style: JSON.parse(key) }));
    }
    // Decorations laid where Chrome lays them inside their box: above the
    // text, or under it when their z-index is below the text's.
    function placeDecorations(el, cs, box, textNode, decos, path, textZ) {
      const [outer, ...inner] = layoutRects([el, ...decos]);
      const borderLeft = px(cs.borderLeftWidth) || 0;
      const borderTop = px(cs.borderTopWidth) || 0;
      decos.forEach((d, k) => {
        const child = build(d, `${path}.children[${box.children.length}]`);
        if (!child) return;
        const r = inner[k];
        const placed = child;
        // A size the author wrote in % stays in % (a clip at 100% of the
        // text it sits on): Chrome's pixels would miss the engine's text.
        const kept = {};
        for (const dim of ["width", "height"]) if (typeof placed.style[dim] === "string" && placed.style[dim].endsWith("%")) kept[dim] = placed.style[dim];
        Object.assign(placed.style, { position: "absolute", left: round(r.left - outer.left - borderLeft, 2),
          top: round(r.top - outer.top - borderTop, 2), width: round(r.width, 2), height: round(r.height, 2) }, kept);
        for (const key of ["right", "bottom", "marginTop", "marginRight", "marginBottom", "marginLeft"]) delete placed.style[key];
        const z = parseInt(getComputedStyle(d).zIndex, 10) || 0;
        if (z < textZ) box.children.splice(textNode ? box.children.indexOf(textNode) : 0, 0, child);
        else box.children.push(child);
      });
    }
    // MW4 -- ::before and ::after placed absolutely in their element (the
    // decorative bars and glows of a card), as boxes, or texts when they
    // write one. In the flow, or generated (a counter, an attribute), they
    // are said and left out.
    function pseudoDecorations(el, cs, path) {
      const out = [];
      for (const which of ["::before", "::after"]) {
        const pcs = getComputedStyle(el, which);
        const content = pcs.content;
        if (!content || content === "none" || content === "normal" || pcs.display === "none") continue;
        if (!positioned(pcs)) { note(path, `pseudo-élément ${which} dans le flux non repris`); continue; }
        if (cs.position === "static") { note(path, `pseudo-élément ${which} placé hors de son élément non repris`); continue; }
        const written_ = content.match(/^"(.*)"$/);
        if (!written_) { note(path, `pseudo-élément ${which} au contenu généré non repris`); continue; }
        const [left, top, w0, h0] = [pcs.left, pcs.top, pcs.width, pcs.height].map(px);
        if ([left, top, w0, h0].some((v) => v === null)) { note(path, `pseudo-élément ${which} sans boîte lisible non repris`); continue; }
        // The format sizes the border box.
        const extra = pcs.boxSizing === "content-box" ? ["Left", "Right", "Top", "Bottom"].map((side) => (px(pcs["padding" + side]) || 0) + (px(pcs[`border${side}Width`]) || 0)) : [0, 0, 0, 0];
        const w = w0 + extra[0] + extra[1];
        const h = h0 + extra[2] + extra[3];
        const s = { position: "absolute", left: round(left, 2), top: round(top, 2), width: round(w, 2), height: round(h, 2) };
        const where = `${path}${which}`;
        if (!transparent(rgb(pcs.backgroundColor))) s.backgroundColor = rgb(pcs.backgroundColor);
        const layers = topLevel(pcs.backgroundImage || "none").filter((l) => l !== "none");
        if (layers.length && /^(linear|radial)-gradient/.test(layers[0])) s.backgroundImage = gradient(layers[0], w, h, where);
        else if (layers.length) note(where, "image de fond non reprise");
        const radius = pcs.borderTopLeftRadius;
        if (radius && radius !== "0px") {
          const r = radius.endsWith("%") ? parseFloat(radius) / 100 * Math.min(w, h) : px(radius);
          if (r) s.borderRadius = round(Math.min(r, Math.min(w, h) / 2));
        }
        const opacity = px(pcs.opacity);
        if (opacity !== 1) s.opacity = round(opacity, 4);
        if (pcs.transform && pcs.transform !== "none") {
          const m = new DOMMatrix(pcs.transform);
          if (round(m.m41, 2)) s.translateX = round(m.m41, 2);
          if (round(m.m42, 2)) s.translateY = round(m.m42, 2);
          const sx = round(Math.hypot(m.a, m.b), 4);
          const sy = round(Math.hypot(m.c, m.d), 4);
          if (sx === sy && sx !== 1) s.scale = sx;
          else if (sx !== sy) { s.scaleX = sx; s.scaleY = sy; }
          const r = round(Math.atan2(m.b, m.a) * 180 / Math.PI, 3);
          if (r) s.rotate = r;
        }
        const f = filter(pcs.filter);
        if (f && f !== "none") s.filter = f;
        else if (f === null) note(where, "filtre non repris");
        if (pcs.boxShadow && pcs.boxShadow !== "none") {
          const m = pcs.boxShadow.match(/^(rgba?\([^)]*\))\s+(-?[\d.]+)px\s+(-?[\d.]+)px\s+([\d.]+)px/);
          if (m && !pcs.boxShadow.includes("inset") && !s.filter) s.filter = `drop-shadow(${m[2]}px ${m[3]}px ${round(Number(m[4]) / 2, 2)}px ${rgb(m[1])})`;
          else note(where, "ombre non reprise");
        }
        if (pcs.mixBlendMode && pcs.mixBlendMode !== "normal") s.mixBlendMode = pcs.mixBlendMode;
        // Its border drawn as an element's is (addBorder), once it is a box.
        const border = borderOf(el, pcs, [w, h]);
        const text = written_[1].replace(/\\([0-9a-f]{1,6})\s?/gi, (_, hex) => String.fromCodePoint(parseInt(hex, 16))).replace(/\\(.)/g, "$1");
        let node;
        if (text.trim()) {
          node = { type: "text", content: text, style: s };
          textStyle(pcs, s);
        } else if (s.backgroundColor || s.backgroundImage || border) {
          s.flexDirection = "column";
          node = { type: "box", style: s };
          if (border) addBorder(node, border, where);
        } else {
          continue;
        }
        if (border && node.type === "text") note(where, "bordure d'un pseudo-élément texte non reprise");
        const z = parseInt(pcs.zIndex, 10) || 0;
        out.push({ which, node, z });
        count++;
      }
      return out;
    }
    // An inline flow whose runs are animated on their own (words or
    // letters split into spans): laid as a wrapping row, one node per run.
    function rowFlow(el, flow) {
      for (const d of el.querySelectorAll("*")) {
        if (animated.has(d) && !flow.decos.some((x) => x.contains(d))) return true;
      }
      return scripted && runsChange(el, flow);
    }
    // MW4 -- runs a script recolours or fades one by one (the words of
    // hw-title whiten as a sweep passes): read at a few instants; a run
    // whose colour or opacity moves makes the text a row of runs, each a
    // node the frame by frame reading follows.
    function runsChange(el, flow) {
      const runs = [...el.children].filter((c) => !flow.decos.includes(c) && !flow.skipped.includes(c));
      if (!runs.length) return false;
      const look = () => runs.map((r) => { const rcs = getComputedStyle(r); return `${rcs.color}|${rcs.opacity}|${rcs.visibility}`; }).join(";");
      const first = look();
      let moved = false;
      for (let k = 1; k <= 8 && !moved; k++) {
        seek(duration / fps * k / 8);
        if (look() !== first) moved = true;
      }
      seek(0);
      return moved;
    }
    // The box of what an element writes, its spaces aside, transforms
    // aside: the gap between two runs is the space Chrome sets between them.
    function contentRects(elements) {
      document.head.appendChild(untransform);
      const rects = elements.map((e) => {
        const texts = [];
        const walker = document.createTreeWalker(e, NodeFilter.SHOW_TEXT);
        for (let n = walker.nextNode(); n; n = walker.nextNode()) if (n.data.trim()) texts.push(n);
        if (!texts.length) return e.getBoundingClientRect();
        const range = document.createRange();
        const first = texts[0];
        const last = texts[texts.length - 1];
        range.setStart(first, first.data.search(/\S/));
        range.setEnd(last, last.data.length - (last.data.length - last.data.trimEnd().length));
        return range.getBoundingClientRect();
      });
      untransform.remove();
      return rects;
    }
    // Pseudo-elements among a box's children: under everything at a
    // negative z-index, ::before ahead of the positioned children (it comes
    // first in the document), ::after on top.
    function addPseudo(node, pseudo) {
      for (const { which, node: child, z } of pseudo) {
        if (z < 0) node.children.unshift(child);
        else if (which === "::before") {
          const k = node.children.findIndex((c) => c.style && c.style.position === "absolute");
          node.children.splice(k < 0 ? node.children.length : k, 0, child);
        } else node.children.push(child);
      }
    }

    // MW4 -- a dashed shape of several sub-paths: Chromium restarts the
    // dash at each sub-path (a connector's two barbs show at once while its
    // curve draws on its own length), the format's trim runs them end to
    // end. Such a shape becomes a box of one path per sub-path, each with
    // its own trim or dash; the box keeps the shape's opacity.
    const splitShapes = new Map(); // shape -> [{node, proxy}]
    // A sub-path's trim is the drawn length clamped to that sub-path, not
    // the tween's curve: its tracks are read frame by frame.
    const splitParts = new Set();
    function splitShape(p, child, matrix) {
      const absolute = bakePath(shapePath(p), new DOMMatrix());
      if (!absolute) return child;
      const subpaths = absolute.split(/(?=M)/).map((x) => x.trim()).filter((x) => /[LCQ]/.test(x));
      if (subpaths.length < 2) return child;
      const d = dashOf(p);
      const box = { type: "box", style: { position: "absolute", left: 0, top: 0, width: child.style.width, height: child.style.height }, children: [] };
      if (child.style.opacity !== undefined) { box.style.opacity = child.style.opacity; }
      const probe = document.createElementNS("http://www.w3.org/2000/svg", "path");
      p.parentNode.appendChild(probe);
      const parts = [];
      for (const sub of subpaths) {
        probe.setAttribute("d", sub);
        const length = probe.getTotalLength();
        const style = { ...child.style };
        delete style.opacity;
        for (const key of ["trimStart", "trimEnd", "strokeDasharray", "strokeDashoffset"]) delete style[key];
        const proxy = { __part: true, el: p, length };
        if (d.list.length) {
          const t = trimOf(p, length);
          if (t) { if (t[0]) style.trimStart = t[0]; if (t[1] !== 1) style.trimEnd = t[1]; }
          else {
            style.strokeDasharray = child.style.strokeDasharray;
            if (child.style.strokeDashoffset) style.strokeDashoffset = child.style.strokeDashoffset;
          }
        }
        const node = { type: "path", d: matrix ? bakePath(sub, matrix) || sub : sub, style };
        if (child.viewBox) node.viewBox = child.viewBox;
        parts.push({ node, proxy });
        splitParts.add(node);
        box.children.push(node);
        count++;
      }
      probe.remove();
      splitShapes.set(p, parts);
      return box;
    }

    // The outermost group between a shape and its svg that a tween moves.
    function movingGroup(p) {
      let found = null;
      for (let e = p.parentNode; e && e.tagName && e.tagName.toLowerCase() !== "svg"; e = e.parentNode) {
        if (animated.has(e) && e.tagName.toLowerCase() === "g") found = e;
      }
      return found;
    }

    function build(el, path, parentInline) {
      const tag = el.tagName.toLowerCase();
      if (["script", "style", "template", "link", "meta", "noscript"].includes(tag)) return null;
      const cs = getComputedStyle(el);
      if (cs.display === "none") return null;
      const spec = specified(el);
      if (tag === "img" || tag === "video" || tag === "canvas" || tag === "iframe") {
        note(path, `${tag} non repris`);
        return null;
      }
      const pseudo = tag === "svg" ? [] : pseudoDecorations(el, cs, path);
      const children = [...el.children].filter((c) => !["script", "style", "template"].includes(c.tagName.toLowerCase()));
      const ownText = [...el.childNodes].filter((n) => n.nodeType === 3 && n.textContent.trim()).length > 0;
      const flow = tag === "svg" ? null : inlineFlow(el);
      let node;
      if (tag === "svg") {
        // Shapes kept for later use (a mask, a clip, a symbol) are not drawn.
        const hidden = (p) => !!p.closest("defs, mask, clipPath, symbol, pattern, marker");
        // display: none on a group hides what it holds (display is not
        // inherited: the shape's own value says nothing).
        const shown = (p) => { for (let e = p; e && e !== el; e = e.parentNode) if (getComputedStyle(e).display === "none") return false; return true; };
        const paths_ = [...el.querySelectorAll(SHAPES)].filter((p) => !hidden(p) && shown(p));
        const drawable = paths_.filter((p) => shapePath(p) !== null);
        // MW4 -- <text> drawn in the svg (a word, a map label).
        const texts = [...el.querySelectorAll("text")].filter((t) => !hidden(t) && shown(t));
        if (!drawable.length && !texts.length) { note(path, "svg sans tracé non repris"); return null; }
        if (drawable.length !== paths_.length) note(path, "formes svg sans tracé (texte, use) non reprises");
        node = { type: "box", style: boxStyle(el, cs, spec, path, false), children: [] };
        const vb = el.getAttribute("viewBox");
        // Its laid-out size (a CSS width of 106% included), transforms aside.
        const w = el.clientWidth || el.width?.baseVal?.value || el.getBoundingClientRect().width;
        const h = el.clientHeight || el.height?.baseVal?.value || el.getBoundingClientRect().height;
        node.style.width = round(w); node.style.height = round(h);
        const shapeNode = (p, k) => {
          const pcs = getComputedStyle(p);
          const ps = { position: "absolute", left: 0, top: 0, width: round(w), height: round(h) };
          const fill = pcs.fill;
          ps.fill = fill === "none" ? "none" : rgb(fill);
          if (pcs.stroke && pcs.stroke !== "none") { ps.stroke = rgb(pcs.stroke); ps.strokeWidth = round(px(pcs.strokeWidth) || 1); }
          if (pcs.strokeLinecap !== "butt") ps.strokeLinecap = pcs.strokeLinecap;
          if (pcs.strokeLinejoin !== "miter") ps.strokeLinejoin = pcs.strokeLinejoin;
          if (ps.stroke) dashStyle(p, ps);
          const child = { type: "path", d: shapePath(p), style: ps };
          // MW4 -- a transform attribute on the shape or a group above it
          // (a frame placed by translate(), letters set along a line)
          // goes into the path data, at the time the tree is read.
          // Inside a group a tween moves, the path holds the transforms
          // below that group; the group's own go to its box.
          const matrix = shapeMatrix(p, movingGroup(p) || el);
          if (matrix) {
            const baked = bakePath(child.d, matrix);
            if (baked) {
              child.d = baked;
              // The stroke and its dashes scale with the shape (on average,
              // under a stretch).
              const k = Math.sqrt(Math.abs(matrix.a * matrix.d - matrix.b * matrix.c));
              if (Math.abs(k - 1) > 1e-6) {
                if (ps.strokeWidth) ps.strokeWidth = round(ps.strokeWidth * k, 3);
                if (ps.strokeDasharray) ps.strokeDasharray = ps.strokeDasharray.split(" ").map((v) => round(Number(v) * k, 3)).join(" ");
                if (ps.strokeDashoffset) ps.strokeDashoffset = round(ps.strokeDashoffset * k, 3);
              }
            }
            else note(`${path}.path[${k}]`, "transform svg non repris (tracé illisible)");
          }
          if (vb) child.viewBox = vb.trim().replace(/\s*,\s*|\s+/g, " ");
          // MW4 -- preserveAspectRatio="none" stretches the drawing to its
          // box; the format always fits it whole (meet). The path gets the
          // box the viewBox fills at the box's height, centred, then is
          // stretched across: the same picture, stroke stretched too.
          const vbs = child.viewBox ? child.viewBox.split(" ").map(Number) : null;
          if (vbs && vbs[2] > 0 && vbs[3] > 0 && el.getAttribute("preserveAspectRatio") === "none" && w > 0 && h > 0) {
            const fitted = vbs[2] * h / vbs[3];
            if (Math.abs(fitted - w) > 0.5) {
              ps.width = round(fitted, 3);
              ps.left = round((w - fitted) / 2, 3);
              ps.scaleX = round(w / fitted, 5);
            }
          }
          // Opacity of the shape, and of its stroke when it has no fill.
          let alpha = px(pcs.opacity);
          if (ps.fill === "none" && ps.stroke) alpha *= px(pcs.strokeOpacity);
          if (ps.stroke && ps.fill !== "none" && px(pcs.strokeOpacity) !== 1) note(`${path}.path[${k}]`, "opacité de contour non reprise");
          if (pcs.visibility === "hidden") alpha = 0;
          if (alpha !== 1) ps.opacity = round(alpha, 4);
          const childPath = `${path}.path[${k}]`;
          // A dashed shape of several sub-paths, split (see splitShape).
          const dashed = ps.stroke && dashOf(p).list.length;
          const shown_ = dashed ? splitShape(p, child, matrix) : child;
          nodes.set(p, shown_); paths.set(p, childPath);
          count++;
          return shown_;
        };
        // MW4 -- a group drawn through an SVG mask (the brush texture of
        // the hw-* pieces, revealed by a white stroke drawn on): an
        // isolated box holding the group's shapes, then the mask's shapes
        // in a box that keeps only what is under it (mask-in). The mask's
        // light shapes are its coverage; its dark ones hide, and are left.
        const maskOf = (p) => {
          for (let e = p.parentNode; e && e !== el; e = e.parentNode) {
            const ref = (e.getAttribute && (e.getAttribute("mask") || e.style?.mask || "")) || "";
            const m = ref.match(/url\(\s*["']?#([^"')]+)["']?\s*\)/);
            if (m) return { holder: e, mask: el.querySelector(`mask#${CSS.escape(m[1])}`) || document.getElementById(m[1]) };
          }
          const own = (p.getAttribute("mask") || "").match(/url\(\s*["']?#([^"')]+)["']?\s*\)/);
          return own ? { holder: p, mask: document.getElementById(own[1]) } : null;
        };
        const groups = new Map(); // masked element -> its box
        let shapeIndex = 0;
        // MW4 -- a group a tween moves (GSAP on a <g>: the arrowheads of
        // hw-arrow) gets a box of the svg's size holding its shapes, which
        // carries the group's opacity and transform, and their animation.
        const vbFit = (() => {
          const v = (el.getAttribute("viewBox") || "").trim().split(/[\s,]+/).map(Number);
          if (v.length !== 4 || !(v[2] > 0) || !(v[3] > 0)) return { k: 1, o: [0, 0] };
          const k = Math.min(w / v[2], h / v[3]);
          return { k, o: [(w - v[2] * k) / 2 - v[0] * k, (h - v[3] * k) / 2 - v[1] * k] };
        })();
        const containerOf = (p) => {
          const g = movingGroup(p);
          if (!g) return node;
          if (!nodes.has(g)) {
            svgGroups.set(g, { svg: el, k: vbFit.k, o: vbFit.o, c: [w / 2, h / 2] });
            const box = { type: "box", style: { position: "absolute", left: 0, top: 0, width: round(w), height: round(h) }, children: [] };
            const t = groupTransform(g);
            if (t.skewed) note(path, "groupe svg cisaillé : cisaillement non repris");
            for (const key of GROUP_TRANSFORM) if (t[key] !== (key.startsWith("scale") ? 1 : 0)) box.style[key] = t[key];
            const gcs = getComputedStyle(g);
            const alpha = gcs.visibility === "hidden" ? 0 : px(gcs.opacity);
            if (alpha !== 1) box.style.opacity = round(alpha, 4);
            nodes.set(g, box); paths.set(g, `${path}.group`);
            count++;
            node.children.push(box);
          }
          return nodes.get(g);
        };
        // A <text> as a text node at the box Chrome gives it in the svg,
        // its size in the svg's units scaled as the viewBox scales. A
        // <textPath> has no equivalent in the format.
        const svgText = (t, cover) => {
          if (t.querySelector("textPath")) { note(path, "texte sur tracé svg (textPath) non repris"); return null; }
          const content = t.textContent.replace(/\s+/g, " ").trim();
          if (!content) return null;
          const tcs = getComputedStyle(t);
          // Its box in its own units, through the transforms between it and
          // the svg (which CSS-level "transform: none" would also undo), into
          // the svg's user units, then into the box as the viewBox fits it.
          const bb = t.getBBox();
          const toSvg = el.getScreenCTM().inverse().multiply(t.getScreenCTM());
          const corners = [[bb.x, bb.y], [bb.x + bb.width, bb.y], [bb.x, bb.y + bb.height], [bb.x + bb.width, bb.y + bb.height]]
            .map(([x, y]) => new DOMPoint(x, y).matrixTransform(toSvg)).map((q) => [vbFit.k * q.x + vbFit.o[0], vbFit.k * q.y + vbFit.o[1]]);
          if (Math.abs(toSvg.b) > 1e-3 || Math.abs(toSvg.c) > 1e-3) note(path, "texte svg tourné : posé droit");
          const scale = Math.hypot(toSvg.a, toSvg.b);
          const r = { left: Math.min(...corners.map((c) => c[0])), top: Math.min(...corners.map((c) => c[1])),
            height: Math.max(...corners.map((c) => c[1])) - Math.min(...corners.map((c) => c[1])) };
          const ts = { position: "absolute", left: round(r.left, 2), top: round(r.top, 2) };
          textStyle(tcs, ts);
          ts.fontSize = round(ts.fontSize * vbFit.k * scale, 3);
          if (ts.letterSpacing) ts.letterSpacing = round(ts.letterSpacing * vbFit.k * scale, 3);
          ts.lineHeight = `${round(r.height, 2)}px`;
          ts.flexShrink = 0;
          const fill = tcs.fill;
          if (cover) ts.color = "rgb(255, 255, 255)";
          else if (!fill || fill === "none" || /url\(/.test(fill)) { note(path, "texte svg sans couleur unie non repris"); return null; }
          else ts.color = rgb(fill);
          let alpha = px(tcs.opacity) * (px(tcs.fillOpacity) ?? 1);
          if (tcs.visibility === "hidden") alpha = 0;
          if (alpha !== 1) ts.opacity = round(alpha, 4);
          delete ts.textAlign;
          const node_ = { type: "text", content, style: ts };
          nodes.set(t, node_); paths.set(t, `${path}.text`);
          count++;
          return node_;
        };
        const items = [...drawable, ...texts].sort((a, b) => (a.compareDocumentPosition(b) & Node.DOCUMENT_POSITION_FOLLOWING ? -1 : 1));
        for (const p of items) {
          if (p.tagName.toLowerCase() === "text") {
            const tn = svgText(p, false);
            if (tn) containerOf(p).children.push(tn);
            continue;
          }
          const child = shapeNode(p, shapeIndex++);
          const container = containerOf(p);
          const masked = maskOf(p);
          if (!masked) { container.children.push(child); continue; }
          if (!masked.mask) { note(path, "masque svg introuvable : tracé dessiné sans lui"); container.children.push(child); continue; }
          if (!groups.has(masked.holder)) {
            const box = { type: "box", style: { position: "absolute", left: 0, top: 0, width: round(w), height: round(h), isolation: "isolate" }, children: [] };
            groups.set(masked.holder, { box, mask: masked.mask });
            container.children.push(box);
          }
          groups.get(masked.holder).box.children.push(child);
        }
        for (const { box, mask } of groups.values()) {
          const cover = { type: "box", style: { position: "absolute", left: 0, top: 0, width: round(w), height: round(h), mixBlendMode: "mask-in" }, children: [] };
          let dark = 0;
          for (const q of mask.querySelectorAll(`${SHAPES}, text`)) {
            if (q.tagName.toLowerCase() === "text") {
              const tn = svgText(q, true);
              if (tn) cover.children.push(tn);
              continue;
            }
            if (shapePath(q) === null) continue;
            const qcs = getComputedStyle(q);
            const paint = qcs.stroke !== "none" ? qcs.stroke : qcs.fill;
            const [r, g, b] = (rgb(paint).match(/[\d.]+/g) || [0, 0, 0]).map(Number);
            if (0.2126 * r + 0.7152 * g + 0.0722 * b < 128) { dark++; continue; }
            const shape = shapeNode(q, shapeIndex++);
            // Coverage only: opaque, whatever colour it has.
            for (const one of shape.type === "path" ? [shape] : shape.children) {
              if (one.style.stroke) one.style.stroke = "rgb(255, 255, 255)";
              if (one.style.fill !== "none") one.style.fill = "rgb(255, 255, 255)";
            }
            cover.children.push(shape);
          }
          if (dark) note(path, "masque svg : formes sombres (qui cachent) non reprises");
          if (cover.children.length) box.children.push(cover);
          else note(path, "masque svg sans forme claire : groupe dessiné sans masque");
        }
      } else if (flow && !ownText && children.some((c) => !flow.decos.includes(c) && !flow.skipped.includes(c)) && rowFlow(el, flow)) {
        // MW4 -- runs animated one by one: one row per line Chrome laid
        // out, each run its own node, the space between runs measured (a
        // space between words is wider than the space between letters).
        node = { type: "box", style: boxStyle(el, cs, spec, path, false), children: [] };
        for (const key of ["justifyContent", "alignItems", "gap", "rowGap", "columnGap", "flexWrap"]) delete node.style[key];
        const along = { center: "center", right: "flex-end", end: "flex-end" }[cs.textAlign];
        const pairs = [];
        children.forEach((c) => {
          if (flow.decos.includes(c) || flow.skipped.includes(c)) return;
          // A span holding a space is the gap after the run before it.
          const ccs = getComputedStyle(c);
          if (!c.children.length && !c.textContent.trim() && transparent(rgb(ccs.backgroundColor)) && !written(specified(c), "width")) return;
          const child = build(c, `${path}.children[${pairs.length}]`);
          if (!child) return;
          pairs.push({ el: c, node: child });
        });
        const rects = contentRects(pairs.map((q) => q.el));
        const lines = [];
        pairs.forEach((q, k) => {
          const r = rects[k];
          const line = lines[lines.length - 1];
          if (!line || r.top > line.top + line.height / 2) lines.push({ top: r.top, height: r.height || 1, items: [k] });
          else line.items.push(k);
        });
        const gaps = [];
        for (const line of lines) {
          for (let i = 1; i < line.items.length; i++) {
            const [a, b] = [line.items[i - 1], line.items[i]];
            const margins = (pairs[a].node.style.marginRight || 0) + (pairs[b].node.style.marginLeft || 0);
            gaps.push({ after: a, width: rects[b].left - rects[a].right - margins });
          }
        }
        const widths = gaps.map((g) => g.width).sort((x, y) => x - y);
        const gap = widths.length ? Math.max(0, widths[Math.floor(widths.length / 4)]) : 0;
        for (const g of gaps) {
          const extra = g.width - gap;
          if (extra > 0.5) pairs[g.after].node.style.marginRight = round((pairs[g.after].node.style.marginRight || 0) + extra, 2);
        }
        const row = (items) => {
          const style = { flexDirection: "row", alignItems: "baseline" };
          if (gap > 0.5) style.columnGap = round(gap, 2);
          if (along) style.justifyContent = along;
          return { type: "box", style, children: items.map((k) => pairs[k].node) };
        };
        if (lines.length <= 1) {
          Object.assign(node.style, row([]).style);
          node.children = pairs.map((q) => q.node);
        } else {
          node.style.flexDirection = "column";
          node.children = lines.map((line) => row(line.items));
        }
        placeDecorations(el, cs, node, null, flow.decos.filter((d) => d.parentElement === el), path, 0);
        addPseudo(node, pseudo);
      } else if (flow && (children.length === 0 || ownText || children.some((c) => !flow.decos.includes(c) && !flow.skipped.includes(c))) &&
          flowText(el, flow).length) {
        // MW4 -- one text in one flow: its spans become ranges, its
        // decorations sibling nodes laid where Chrome lays them.
        const chars = flowText(el, flow);
        const textNode = { type: "text", content: chars.map((c) => c.ch).join(""), style: {} };
        if (children.length) {
          const ranges = rangesOf(chars, el, cs, path);
          if (ranges.length) textNode.ranges = ranges;
          mixedTexts.set(el, path);
        }
        // A text centred in a box of its own size (an avatar's initials)
        // keeps that box: a text's height is its lines.
        const display = cs.display;
        const centred = ["flex", "inline-flex", "grid", "inline-grid"].includes(display) &&
          (written(spec, "height") || written(spec, "aspect-ratio")) || !!borderOf(el, cs);
        if (flow.decos.length || pseudo.length || centred) {
          node = { type: "box", style: boxStyle(el, cs, spec, path, false), children: [textNode] };
          textStyle(cs, textNode.style);
          wrappedText.set(node, textNode);
          const textZ = Math.max(0, ...[...new Set(chars.map((c) => c.owner))].map((o) => {
            const ocs = getComputedStyle(o);
            return o !== el && ocs.position !== "static" ? parseInt(ocs.zIndex, 10) || 0 : 0;
          }));
          placeDecorations(el, cs, node, textNode, flow.decos, path, textZ);
          addPseudo(node, pseudo);
        } else {
          Object.assign(textNode.style, boxStyle(el, cs, spec, path, true));
          textStyle(cs, textNode.style);
          node = textNode;
        }
      } else if (children.length === 0 || (flow && !flow.decos.length && children.every((c) => flow.skipped.includes(c)))) {
        node = { type: "box", style: boxStyle(el, cs, spec, path, false), children: [] };
        addPseudo(node, pseudo);
        if (!node.children.length) delete node.children;
        if (!node.style.width && !node.style.height && !node.style.backgroundColor && !node.style.backgroundImage && !node.children) {
          // An empty element without size or paint draws nothing.
          const r = el.getBoundingClientRect();
          if (!r.width || !r.height) return null;
        }
      } else {
        if (ownText) note(path, "texte à côté d'éléments : texte non repris");
        node = { type: "box", style: boxStyle(el, cs, spec, path, false), children: [] };
        const grid = cs.display === "grid" || cs.display === "inline-grid";
        const pairs = [];
        children.forEach((c) => {
          const child = build(c, `${path}.children[${node.children.length}]`);
          if (!child) return;
          node.children.push(child);
          pairs.push({ el: c, node: child });
        });
        if (grid) layoutGridChildren(el, cs, node, pairs);
        addPseudo(node, pseudo);
      }
      if (!node) return null;
      if (node.type === "box" && pendingBorders.has(node.style)) addBorder(node, pendingBorders.get(node.style), path);
      staticTransform(el, node.style);
      const opacity = px(cs.opacity);
      if (opacity !== 1 && !ownHidden(el)) node.style.opacity = round(opacity, 4);
      // visibility is inherited: only the element that hides itself is
      // made transparent, or a parent shown later leaves its children out.
      if (ownHidden(el)) node.style.opacity = 0;
      const label = el.id || (typeof el.className === "string" && el.className.split(/\s+/).find((c) => c && c !== "clip")) || "";
      if (label) {
        // Node ids are unique in a composition; a repeated class is numbered.
        const base = label.replace(/[^\w-]/g, "_").slice(0, 44);
        let unique = base;
        for (let k = 2; ids.has(unique); k++) unique = `${base}-${k}`;
        ids.add(unique);
        node.id = unique;
      }
      nodes.set(el, node);
      paths.set(el, path);
      count++;
      // A HyperFrames clip shows only inside its window: a box around it
      // whose opacity steps on at its start and off at its end.
      if (el.hasAttribute("data-start") && el !== root) {
        const start = Number(el.getAttribute("data-start"));
        const length = Number(el.getAttribute("data-duration"));
        if (!Number.isFinite(start)) {
          note(path, `data-start « ${el.getAttribute("data-start")} » non lu : visible tout le long`);
        } else {
          const end = Number.isFinite(length) ? start + length : Infinity;
          const total = duration / fps;
          if (start > 1e-6 || end < total - 1e-6) {
            const frames = [];
            const values = [];
            if (start > 1e-6) { frames.push(0, framesOf(start, fps)); values.push(0, 1); }
            else { frames.push(0); values.push(1); }
            if (end < total - 1e-6) { frames.push(framesOf(end, fps)); values.push(0); }
            const wrapper = {
              type: "box",
              style: { position: "absolute", top: 0, left: 0, width, height },
              animate: { opacity: { interpolate: { frames, values, easing: "step-end" } } },
              children: [node],
            };
            if (frames.length === 1) delete wrapper.animate.opacity.interpolate.easing;
            if (node.style.position !== "absolute") {
              Object.assign(node.style, { position: "absolute", top: 0, left: 0, width, height });
            }
            return wrapper;
          }
        }
      }
      return node;
    }

    const rootStyle = { width, height };
    const rootNode = { type: "fill", style: {}, children: [] };
    const rootCs = getComputedStyle(root);
    // MW4 -- a component mounted by data-composition-src: the root read is
    // the demo's mount card. Only its size belongs to the piece; its
    // background, shadow and corners are the demo's dressing, and would
    // lay a dark veil over any film the piece is put on.
    const mountedHost = root.hasAttribute("data-composition-src") || !!root.querySelector(":scope > [data-hf-inner-root]");
    if (mountedHost) note("root", "habillage de la démo (fond, ombre, coins) laissé hors de la pièce");
    if (!mountedHost && !transparent(rgb(rootCs.backgroundColor))) rootNode.style.backgroundColor = rgb(rootCs.backgroundColor);
    const rootLayers = topLevel(rootCs.backgroundImage || "none").filter((l) => l !== "none");
    if (!mountedHost && rootLayers.length && /^(linear|radial)-gradient/.test(rootLayers[0])) rootNode.style.backgroundImage = gradient(rootLayers[0], width, height, "root");
    // The root's own layout: often the grid or flexbox that centres
    // everything. Its size and place are the composition's.
    const rootLayout = boxStyle(root, rootCs, specified(root), "root", false);
    for (const k of ["position", "top", "right", "bottom", "left", "width", "height", "minWidth", "minHeight",
      "maxWidth", "maxHeight", "marginTop", "marginRight", "marginBottom", "marginLeft", "backgroundColor",
      "backgroundImage", "borderRadius", "overflow", "flexGrow", "flexShrink", "alignSelf"]) delete rootLayout[k];
    if (mountedHost) { delete rootLayout.filter; delete rootLayout.mixBlendMode; }
    Object.assign(rootNode.style, rootLayout);
    const rootGrid = rootCs.display === "grid" || rootCs.display === "inline-grid";
    const rootPairs = [];
    [...root.children].forEach((c) => {
      const child = build(c, `root.children[${rootNode.children.length}]`);
      if (child) {
        // A HyperFrames clip fills its composition.
        if (c.classList.contains("clip") && !child.style.position) {
          Object.assign(child.style, { position: "absolute", top: 0, left: 0, width, height });
        }
        rootNode.children.push(child);
        rootPairs.push({ el: c, node: child });
      }
    });
    if (rootGrid) layoutGridChildren(root, rootCs, rootNode, rootPairs);
    // The mount card's border and shadow were left out with it: not gaps.
    if (mountedHost) {
      for (let k = notes.length - 1; k >= 0; k--) if (/^root: (bordure|ombre)/.test(notes[k])) notes.splice(k, 1);
    }

    // --- the timeline ----------------------------------------------------
    const leaves = [];
    let imperative = 0;
    // FI3 -- ROADMAP.md. Child durations live in their own clock, and a
    // repeated tween occupies all its cycles (repeatDelay included). A
    // repeating/reversed ancestor maps several local times to one track:
    // read that track over the composition instead of inventing its keys.
    const walk = (timeline, offset, scale, parentBaked = false) => {
      const inheritedBake = parentBaked || !!timeline.repeat();
      for (const c of timeline.getChildren(false, true, true)) {
        const start = offset + c.startTime() / scale;
        const speed = c.timeScale();
        const bake = inheritedBake || speed <= 0 || c.paused();
        const childScale = scale * (Math.abs(speed) || 1);
        if (c.getChildren) { walk(c, start, childScale, bake); continue; }
        if (c.timeline && c.timeline.getChildren) { walk(c.timeline, start, childScale, true); continue; }
        const v = c.vars || {};
        if (typeof v.onUpdate === "function") imperative++;
        leaves.push({ tween: c, start: bake ? 0 : start,
          end: bake ? duration / fps : start + c.totalDuration() / childScale,
          bake, repeat: c.repeat(), yoyo: !!v.yoyo });
      }
    };
    // seek(t) addresses the root's own clock, regardless of its timeScale.
    walk(tl, 0, 1);
    // A callback on a timeline itself (tl.eventCallback("onUpdate", ...)),
    // this one or any the page registers, drives effects just the same.
    const driven = (t) => t && t.eventCallback && typeof t.eventCallback("onUpdate") === "function";
    for (const t of Object.values(timelines)) if (driven(t)) imperative++;
    const nested = (t) => { for (const c of t.getChildren ? t.getChildren(true, false, true) : []) { if (driven(c)) imperative++; } };
    nested(tl);

    const read = (el, prop) => {
      if (el && el.__part) {
        if (prop === "trimStart" || prop === "trimEnd") {
          const t = trimOf(el.el, el.length);
          return t ? t[prop === "trimStart" ? 0 : 1] : null;
        }
        if (prop === "strokeDashoffset") return round(dashOf(el.el).offset, 3);
        return null;
      }
      const g = window.gsap;
      const cs = getComputedStyle(el);
      switch (prop) {
        case "translateX": if (svgGroups.has(el)) return groupTransform(el).translateX; return round(translation(el)[0], 3);
        case "translateY": if (svgGroups.has(el)) return groupTransform(el).translateY; return round(translation(el)[1], 3);
        case "scale": return round(g.getProperty(el, "scaleX"), 5);
        case "scaleX": if (svgGroups.has(el)) return groupTransform(el).scaleX; return round(g.getProperty(el, "scaleX"), 5);
        case "scaleY": if (svgGroups.has(el)) return groupTransform(el).scaleY; return round(g.getProperty(el, "scaleY"), 5);
        case "rotate": if (svgGroups.has(el)) return groupTransform(el).rotate; return round(g.getProperty(el, "rotation"), 4);
        case "rotateX": return round(g.getProperty(el, "rotationX"), 4);
        case "rotateY": return round(g.getProperty(el, "rotationY"), 4);
        case "opacity": return ownHidden(el) ? 0 : round(px(cs.opacity), 5);
        case "color": case "backgroundColor": case "fill": case "stroke": return rgb(cs[prop]);
        case "clipPath": return cs.clipPath;
        case "filter": return filter(cs.filter);
        case "trimStart": case "trimEnd": {
          const t = trimOf(el);
          return t ? t[prop === "trimStart" ? 0 : 1] : null;
        }
        case "strokeDashoffset": return round(dashOf(el).offset, 3);
        default: { const v = px(cs[prop]); return v === null ? null : round(v, 3); }
      }
    };

    // The shapes a dash tween moves: the target itself, or for a CSS
    // variable the shapes under it whose dash changes over the tween.
    function dashShapes(el, leaf, viaVariable) {
      const candidates = (el.matches && el.matches(SHAPES) ? [el] : [...(el.querySelectorAll ? el.querySelectorAll(SHAPES) : [])])
        .filter((p) => nodes.has(p));
      if (!viaVariable) return candidates;
      const sign = (p) => { const cs = getComputedStyle(p); return `${cs.strokeDasharray}|${cs.strokeDashoffset}`; };
      seek(Math.max(0, leaf.start - 1e-4));
      const before = candidates.map(sign);
      seek(leaf.end + 1e-7);
      return candidates.filter((p, k) => sign(p) !== before[k]);
    }
    const lostTargets = new Set();
    const tracks = new Map(); // node -> Map(prop -> segments)
    const unmapped = new Map();
    let baked = 0;
    for (const leaf of leaves) {
      const { tween } = leaf;
      const targets = (tween.targets ? tween.targets() : []).filter((t) => t instanceof Element);
      const vars = tween.vars || {};
      const props = new Set();
      for (const key of Object.keys(vars)) {
        if (CONTROL.has(key)) continue;
        if (key === "css") { for (const k of Object.keys(vars.css)) props.add(k); continue; }
        if (key === "attr" && vars.attr && typeof vars.attr === "object") {
          for (const k of Object.keys(vars.attr)) props.add(DASH.has(k) ? "strokeDashoffset" : `attr.${k}`);
          continue;
        }
        props.add(key);
      }
      const ease = easeName(tween);
      const addTrack = (node, el, prop) => {
        // A box that wraps a text hands its typography down to it.
        if (TEXT_PROPS.has(prop) && wrappedText.has(node)) node = wrappedText.get(node);
        if (!tracks.has(node)) tracks.set(node, new Map());
        const byProp = tracks.get(node);
        if (!byProp.has(prop)) byProp.set(prop, { el, segments: [] });
        byProp.get(prop).segments.push({ ...leaf, ease });
      };
      for (const el of targets) {
        const node = nodes.get(el);
        for (const p of props) {
          // MW4 -- a dash, or a CSS variable a dash reads: a trim on each
          // shape it moves, or the dash itself when it is a real dash.
          if (DASH.has(p) || p.startsWith("--")) {
            const shapes = dashShapes(el, leaf, p.startsWith("--"));
            if (!shapes.length) { unmapped.set(p, (unmapped.get(p) || 0) + 1); continue; }
            for (const shape of shapes) {
              seek(leaf.end + 1e-7);
              const d = dashOf(shape);
              const shapeNode = nodes.get(shape);
              if (splitShapes.has(shape)) {
                for (const { node: part, proxy } of splitShapes.get(shape)) {
                  if (d.list.length && trimOf(shape, proxy.length) === null) {
                    if (!part.style.strokeDasharray) part.style.strokeDasharray = d.list.map((v) => round(v, 3)).join(" ");
                    addTrack(part, proxy, "strokeDashoffset");
                  } else {
                    addTrack(part, proxy, "trimStart");
                    addTrack(part, proxy, "trimEnd");
                  }
                }
                continue;
              }
              if (d.list.length && !d.drawOn) {
                if (!shapeNode.style.strokeDasharray) shapeNode.style.strokeDasharray = d.list.map((v) => round(v, 3)).join(" ");
                addTrack(shapeNode, shape, "strokeDashoffset");
              } else {
                addTrack(shapeNode, shape, "trimStart");
                addTrack(shapeNode, shape, "trimEnd");
              }
            }
            continue;
          }
          if (!node) {
            // Inside a text drawn as one (a span of a mixed text), or a
            // node left out: its animation goes with it, said once.
            const holder = el.closest && [...mixedTexts.keys()].find((t) => t !== el && t.contains(el));
            lostTargets.add(holder ? `${mixedTexts.get(holder)}: passage de texte animé (${p}) : animation non reprise` : null);
            continue;
          }
          let prop = PROPERTY[p];
          if (!prop) { unmapped.set(p, (unmapped.get(p) || 0) + 1); continue; }
          if (svgGroups.has(el) && ["translateX", "translateY", "scale", "scaleX", "scaleY", "rotate"].includes(prop)) {
            // About the group's own origin, which the box centre is not:
            // translation and rotation are tied, so read at every frame.
            for (const q of GROUP_TRANSFORM) addTrack(node, el, q);
            forcedBake.add(node);
            continue;
          }
          if (prop === "scale") {
            seek(leaf.start); const a = window.gsap.getProperty(el, "scaleX") === window.gsap.getProperty(el, "scaleY");
            if (!a) prop = "scaleX";
          }
          addTrack(node, el, prop);
          if (p === "scale" && prop === "scaleX") addTrack(node, el, "scaleY");
        }
      }
    }
    for (const what of lostTargets) if (what) notes.push(what);
    for (const [p, n] of unmapped) note("timeline", `${n} animation(s) de « ${p} » sans équivalent`);
    if (imperative) note("timeline", `${imperative} tween(s) à onUpdate : leur effet n'est pas repris`);

    const frameTimes = (t) => framesOf(t, fps);
    for (const [node, byProp] of tracks) {
      for (const [prop, { el, segments }] of byProp) {
        segments.sort((a, b) => a.start - b.start || a.end - b.end);
        let overlap = (forcedBake.has(node) && GROUP_TRANSFORM.includes(prop)) || splitParts.has(node) ||
          segments.some((s, k) => k && s.start < segments[k - 1].end - 1e-6) ||
          segments.some((s) => s.bake || s.repeat || s.yoyo || (s.ease.kind === "bake" && s.end - s.start > 1e-9));
        const keys = []; // {frame, value, easing}
        const push = (t, value, easing) => {
          const frame = frameTimes(t);
          const last = keys[keys.length - 1];
          // Instants less than 1e-4 frame apart are one key: read as two
          // fractions they could come out in the wrong order.
          if (last && Math.abs(frameValue(last.frame) - frameValue(frame)) < 1e-4) { last.value = value; if (easing) last.easing = easing; return; }
          keys.push({ frame, value, easing: easing || "linear" });
        };
        if (!overlap) {
          for (const s of segments) {
            if (s.end - s.start < 1e-9) {
              // A set: the value before it holds until its frame, then the
              // new one. Without the key before, the new value would also
              // hold backwards to frame 0.
              seek(Math.max(0, s.start - 1e-4)); const before = read(el, prop);
              seek(s.start + 1e-7); const after = read(el, prop);
              if (before === null || after === null) { overlap = true; break; }
              const last = keys[keys.length - 1];
              if (s.start * fps < 0.5) { push(0, after, null); continue; }
              if (!last) push(0, before, "step-end");
              else if (JSON.stringify(last.value) !== JSON.stringify(after)) last.easing = "step-end";
              push(s.start, after, null);
              continue;
            }
            seek(s.start); const v0 = read(el, prop);
            seek(s.end); const v1 = read(el, prop);
            if (v0 === null || v1 === null) { overlap = true; break; }
            const last = keys[keys.length - 1];
            if (last && JSON.stringify(last.value) !== JSON.stringify(v0)) {
              // A jump between segments (an immediate render): the value
              // holds, then changes on the segment's first frame.
              last.easing = "step-end";
            }
            push(s.start, v0, s.ease.easing);
            push(s.end, v1, null);
          }
        }
        if (overlap) {
          // Overlapping tweens, a repeat or an unnamed curve: every frame
          // the union covers, as GSAP plays it.
          keys.length = 0;
          const from = Math.max(0, Math.floor(Math.min(...segments.map((s) => s.start)) * fps));
          const to = Math.min(duration, Math.ceil(Math.max(...segments.map((s) => s.end)) * fps));
          for (let f = from; f <= to; f++) {
            seek(f / fps);
            const v = read(el, prop);
            if (v !== null) keys.push({ frame: f, value: v, easing: "linear" });
          }
          baked++;
          note(paths.get(el) || "?", `${prop} relevé image par image`);
        }
        if (!keys.length) continue;
        // A track that only ever says "none" changes nothing.
        if (keys.every((k) => k.value === "none")) continue;
        if (prop === "clipPath" && keys.some((k) => k.value === "none")) {
          // MW4 -- "none" between shapes: the shape of the same kind that
          // clips nothing, with as many vertices as the track's polygons.
          const shape = keys.find((k) => k.value !== "none").value;
          const kind = shape.slice(0, shape.indexOf("("));
          let open = null;
          if (kind === "circle") open = "circle(100% at 50% 50%)";
          else if (kind === "ellipse") open = "ellipse(100% 100% at 50% 50%)";
          else if (kind === "inset") open = "inset(0px)";
          else if (kind === "polygon") {
            const n = topLevel(shape.slice(shape.indexOf("(") + 1, -1)).length;
            const corners = ["0% 0%", "100% 0%", "100% 100%", "0% 100%"];
            open = `polygon(${Array.from({ length: n }, (_, i) => corners[Math.min(i, 3)]).join(", ")})`;
          }
          if (!open) { note(paths.get(el) || "?", "découpe animée vers « none » non reprise"); continue; }
          for (const k of keys) if (k.value === "none") k.value = open;
        }
        if (prop === "filter") {
          if (keys.some((k) => k.value === null)) { note(paths.get(el) || "?", "filtre animé non lu"); continue; }
          const shape = keys.find((k) => k.value !== "none");
          if (shape) {
            const zero = shape.value.replace(/blur\([^)]*\)/g, "blur(0px)")
              .replace(/drop-shadow\([^)]*\)/g, "drop-shadow(0px 0px 0px rgba(0, 0, 0, 0))");
            for (const k of keys) if (k.value === "none") k.value = zero;
            const names = (v) => v.replace(/\([^)]*\)/g, "");
            if (keys.some((k) => names(k.value) !== names(shape.value))) {
              note(paths.get(el) || "?", "filtre animé dont la pile change de forme : non repris");
              continue;
            }
          }
        }
        // Drop keys that change nothing between equal neighbours.
        const kept = keys.filter((k, i) => i === 0 || i === keys.length - 1 ||
          JSON.stringify(k.value) !== JSON.stringify(keys[i - 1].value) ||
          JSON.stringify(k.value) !== JSON.stringify(keys[i + 1].value));
        // A track that holds one value all along is a style.
        if (kept.every((k) => JSON.stringify(k.value) === JSON.stringify(kept[0].value))) {
          node.style[prop] = kept[0].value;
          continue;
        }
        node.animate = node.animate || {};
        const interpolate = { frames: kept.map((k) => k.frame), values: kept.map((k) => k.value) };
        if (kept.length > 1) {
          const curves = kept.slice(0, -1).map((k) => k.easing);
          interpolate.easing = curves.every((c) => c === curves[0]) ? curves[0] : curves;
          if (interpolate.easing === "linear") delete interpolate.easing;
        }
        node.animate[prop] = { interpolate };
        delete node.style[prop];
      }
    }
    // MW4 -- effects a page drives in JavaScript (an onUpdate that sets a
    // dash, an opacity, a transform) are no tween's: when the page has
    // any, every node is read at every frame, and what moves without a
    // track gets one, frame by frame. SVG shapes are read for their trim,
    // dash and opacity only (their transform is baked into their path).
    if (imperative) {
      const watched = [];
      for (const [el, n] of nodes) {
        if (!(el instanceof Element)) continue;
        const shape = el.matches(SHAPES);
        const list = shape ? ["opacity", "trimStart", "trimEnd", "strokeDashoffset"]
          : ["opacity", "translateX", "translateY", "scaleX", "scaleY", "rotate"];
        const owner = n;
        const free = list.filter((q) => !(owner.animate && owner.animate[q]) &&
          !(q.startsWith("scale") && owner.animate && owner.animate.scale) &&
          !(q === "strokeDashoffset" && !(owner.style && owner.style.strokeDasharray)) &&
          !(q.startsWith("trim") && owner.style && owner.style.strokeDasharray));
        if (splitShapes.has(el)) {
          // The box takes the opacity, each sub-path its trim or dash.
          if (!(owner.animate && owner.animate.opacity)) watched.push({ el, node: owner, props: ["opacity"], values: [[]] });
          for (const { node: part, proxy } of splitShapes.get(el)) {
            const qs = ["trimStart", "trimEnd", "strokeDashoffset"].filter((q) => !(part.animate && part.animate[q]) &&
              !(q === "strokeDashoffset" && !part.style.strokeDasharray) && !(q.startsWith("trim") && part.style.strokeDasharray));
            if (qs.length) watched.push({ el: proxy, node: part, props: qs, values: qs.map(() => []) });
          }
          continue;
        }
        if (free.length) watched.push({ el, node: owner, props: free, values: free.map(() => []) });
        // A text's colour, on the text itself when a box wraps it.
        const text = n.type === "text" ? n : wrappedText.get(n);
        if (text && !(text.animate && text.animate.color)) watched.push({ el, node: text, props: ["color"], values: [[]] });
      }
      // One computed style per element and frame, every property drawn
      // from it (a shape's length is measured once, in dashOf).
      const sample = (el, q, cache) => {
        if (el.__part) return read(el, q);
        let v = cache.get(el);
        if (!v) {
          const cs = getComputedStyle(el);
          v = { cs };
          if (svgGroups.has(el)) Object.assign(v, groupTransform(el));
          else if (!el.matches(SHAPES)) {
            const m = cs.transform && cs.transform !== "none" ? new DOMMatrix(cs.transform) : new DOMMatrix();
            v.translateX = round(m.m41, 3); v.translateY = round(m.m42, 3);
            v.scaleX = round(Math.hypot(m.a, m.b), 5); v.scaleY = round(Math.hypot(m.c, m.d), 5);
            v.rotate = round(Math.atan2(m.b, m.a) * 180 / Math.PI, 4);
          }
          cache.set(el, v);
        }
        if (q === "opacity") return ownHidden(el) ? 0 : round(px(v.cs.opacity), 5);
        if (q === "color") return rgb(v.cs.color);
        if (q in v) return v[q];
        return read(el, q);
      };
      const started = performance.now();
      let complete = true;
      try {
        for (let f = 0; f <= duration; f++) {
          if (performance.now() - started > 20000) { complete = false; break; }
          seek(f / fps);
          const cache = new Map();
          for (const w of watched) w.props.forEach((q, i) => w.values[i].push(sample(w.el, q, cache)));
        }
      } catch (e) {
        // The page's own callback failed on a seek (a canvas API Chromium
        // lacks): the tween reading stands, the frame by frame one goes.
        complete = false;
        playhead = null;
      }
      if (!complete) {
        note("timeline", "effets pilotés en JavaScript : relevé image par image trop long ou en erreur, abandonné");
      } else {
        let found = 0;
        for (const w of watched) {
          w.props.forEach((q, i) => {
            const vs = w.values[i];
            if (vs.some((v) => v === null) || vs.every((v) => JSON.stringify(v) === JSON.stringify(vs[0]))) return;
            // Keys where the value changes, with the frames around a hold.
            const frames = [];
            const values = [];
            vs.forEach((v, f) => {
              const same = (a, b) => JSON.stringify(a) === JSON.stringify(b);
              if (f === 0 || f === vs.length - 1 || !same(v, vs[f - 1]) || !same(v, vs[f + 1])) { frames.push(f); values.push(v); }
            });
            w.node.animate = w.node.animate || {};
            w.node.animate[q] = { interpolate: { frames, values } };
            delete w.node.style[q];
            if (q.startsWith("scale") && w.node.style.scale !== undefined) {
              w.node.style.scaleX = w.node.style.scaleY = w.node.style.scale;
              delete w.node.style.scale;
              delete w.node.style[q];
            }
            found++;
          });
        }
        if (found) {
          // What onUpdate did to these nodes is now kept: its note goes.
          const k = notes.findIndex((n) => n.startsWith("timeline: ") && n.includes("onUpdate"));
          if (k >= 0) notes.splice(k, 1);
          note("timeline", `effets pilotés en JavaScript relevés image par image (${found} piste(s)) ; texte réécrit en script non repris`);
        }
      }
    }
    seek(0);
    if (seekErrors) note("timeline", `script de la page en erreur sur ${seekErrors} lecture(s) : effets qu'il dessine non repris`);

    // --- props -----------------------------------------------------------
    const props = {};
    const main = [...fontCount.entries()].sort((a, b) => b[1] - a[1])[0];
    if (main) {
      props.police = { type: "text", default: main[0] };
      rootNode.style.fontFamily = { prop: "police" };
      const strip = (n) => {
        if (n.type === "text" && n.style.fontFamily === main[0]) delete n.style.fontFamily;
        (n.children || []).forEach(strip);
      };
      strip(rootNode);
    }
    // Every text becomes a prop named after its element, so a piece is
    // filled without editing its tree.
    const used = new Set(Object.keys(props));
    const texts = [];
    // A wrapped text is named after its box.
    const wrapperOf = new Map([...wrappedText].map(([box, text]) => [text, box]));
    const visit = (n) => { if (n.type === "text") texts.push(n); (n.children || []).forEach(visit); };
    visit(rootNode);
    if (texts.length <= 12) {
      texts.forEach((n, k) => {
        let name = (n.id || wrapperOf.get(n)?.id || `texte_${k + 1}`).toLowerCase().replace(/[^a-z0-9_]/g, "_").replace(/^_+|_+$/g, "") || `texte_${k + 1}`;
        while (used.has(name)) name += "_";
        used.add(name);
        props[name] = { type: "text", default: n.content };
        n.content = { prop: name };
      });
    } else {
      note("props", `${texts.length} textes : laissés écrits, pas en props`);
    }

    const composition = { width, height, fps, duration };
    if (Object.keys(props).length) composition.props = props;
    composition.root = rootNode;
    return {
      composition,
      notes,
      stats: { nodes: count, tweens: leaves.length, baked, origins: nodes.__origins || 0, texts: texts.length, svgGroups: svgGroups.size },
      variables: root.getAttribute("data-composition-variables") || null,
    };
  };
})();
