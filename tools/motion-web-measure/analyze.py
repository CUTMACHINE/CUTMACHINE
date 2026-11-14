"""Classify traced GSAP timelines against the CUTMACHINE motion format.

MOTION-WEB measurement, 2026-10-11. Property-name level only: values
(clipPath shapes, filter functions, units) are not checked.

Usage: python3 -I analyze.py all.jsonl [--detail NAME]
"""
import collections
import json
import re
import sys

DIRECT = {
    "x": "translateX", "y": "translateY", "scale": "scale", "scaleX": "scaleX",
    "scaleY": "scaleY", "rotation": "rotate", "rotate": "rotate",
    "rotationZ": "rotate", "rotationX": "rotateX", "rotationY": "rotateY",
    "opacity": "opacity", "autoAlpha": "opacity", "color": "color",
    "backgroundColor": "backgroundColor", "fill": "fill", "stroke": "stroke",
    "strokeWidth": "strokeWidth", "letterSpacing": "letterSpacing",
    "wordSpacing": "wordSpacing", "clipPath": "clipPath", "filter": "filter",
    "visibility": "visible/opacity", "display": "visible",
    "width": "width", "height": "height",
}
# Translatable, but only by resolving against the laid-out box or by an idiom.
CONVERT = {
    "xPercent": "translateX (box)", "yPercent": "translateY (box)",
    "top": "inset", "left": "inset", "right": "inset", "bottom": "inset",
    "strokeDashoffset": "trim", "strokeDasharray": "trim",
    "drawSVG": "trim", "css.strokeDashoffset": "trim",
    "fontSize": "fontSize", "borderRadius": "borderRadius",
    "padding": "padding", "margin": "margin", "gap": "gap",
    "transformOrigin": "MB6 refus", "svgOrigin": "MB6 refus",
    "textShadow": "filter drop-shadow/glow", "boxShadow": "filter drop-shadow",
}

POLY = re.compile(r"^(power[0-4]|quad|cubic|quart|quint|strong|sine|expo|circ)"
                  r"(\.(in|out|inOut))?$")


def ease_class(ease, dur):
    if dur == 0:
        return "hold"
    e = ease.replace(" ", "").rstrip("*")
    if e in ("none", "linear", "power0", "power0.none", "power0.in",
             "power0.out", "power0.inOut"):
        return "exact"
    if e in ("steps(1)",):
        return "exact"
    if e == "default":
        return "poly"  # GSAP default is power1.out
    if POLY.match(e):
        return "poly"
    if e.startswith("back"):
        return "back"
    return "bake"  # elastic, bounce, steps(n), CustomEase, functions


def prop_class(name, kind):
    if kind in ("function", "random"):
        return "dynamic"
    if name in DIRECT:
        return "direct"
    if name in CONVERT:
        return "transformOrigin" if "MB6" in CONVERT[name] else "convert"
    return "absent"


def on_frame(t, fps):
    return abs(t * fps - round(t * fps)) < 1e-6


def main():
    path = sys.argv[1]
    detail = sys.argv[3] if len(sys.argv) > 3 and sys.argv[2] == "--detail" else None
    recs = [json.loads(l) for l in open(path)]
    totals = collections.Counter()
    pc = collections.Counter()
    absent = collections.Counter()
    eases = collections.Counter()
    ease_names = collections.Counter()
    stag = collections.Counter()
    buckets = collections.defaultdict(list)
    per = []
    for r in recs:
        name = f'{r["kind"]}/{r["name"]}'
        if r.get("failed"):
            buckets["échec de chargement"].append(name)
            continue
        tw = r.get("tweens", [])
        if not tw:
            reason = "aucune timeline" if not r.get("timelines") else "timeline vide"
            buckets[reason].append(name)
            continue
        c = collections.Counter()
        frames = collections.Counter()
        manual = 0
        for t in tw:
            if t["dur"] > 0:
                ec = ease_class(t["ease"], t["dur"])
                eases[ec] += 1
                ease_names[t["ease"]] += 1
            for fps in (25, 30, 50, 60):
                ok = on_frame(t["start"], fps) and on_frame(t["start"] + t["dur"], fps)
                frames[(fps, ok)] += 1
            tgt = t.get("target") or {}
            if tgt.get("kind") != "element":
                c["non-element"] += 1
            if tgt.get("leafText") and tgt.get("sameClassSiblings", 0) > 2 and not t.get("stagger"):
                manual += 1
            if t.get("stagger") is not None:
                s = t["stagger"]
                frm = s.get("from", "start") if isinstance(s, dict) else "start"
                stag["grid" if isinstance(s, dict) and "grid" in s else str(frm)] += 1
            if t.get("onUpdate"):
                c["onUpdate"] += 1
            for p, kind in t["props"].items():
                k = prop_class(p, kind)
                c[k] += 1
                pc[k] += 1
                if k == "absent":
                    absent[p] += 1
            if t.get("transformOrigin") and t["transformOrigin"] not in ("50% 50%", "center", "center center", "50% 50% 0px"):
                c["transformOriginStyle"] += 1
        props = sum(c[k] for k in ("direct", "convert", "absent", "dynamic", "transformOrigin"))
        structural = (c["direct"] + c["convert"]) / props if props else 0
        dom = r.get("dom", {})
        painted = dom.get("canvas", 0) > 0 or c["onUpdate"] > 0 or c["non-element"] > 0
        canvas = dom.get("canvas", 0) > 0
        exact25 = frames[(25, True)] / max(1, frames[(25, True)] + frames[(25, False)])
        exact30 = frames[(30, True)] / max(1, frames[(30, True)] + frames[(30, False)])
        row = dict(name=name, tweens=len(tw), props=props, structural=structural,
                   painted=painted, canvas=dom.get("canvas", 0), onUpdate=c["onUpdate"],
                   nonElement=c["non-element"], absent=c["absent"], dynamic=c["dynamic"],
                   origin=c["transformOrigin"] + c["transformOriginStyle"],
                   manual=manual, exact25=exact25, exact30=exact30, tags=r.get("tags", []))
        per.append(row)
        if canvas:
            b = "C2 : canvas / WebGL (pré-rendu seulement)"
        elif painted:
            b = "C1 : DOM piloté en JS (onUpdate, objet animé)"
        elif structural >= 0.999 and row["origin"] == 0:
            b = "A : tout se traduit en structure"
        elif structural >= 0.9:
            b = "B : presque (≥ 90 % des propriétés)"
        else:
            b = "B' : partiel (< 90 %)"
        buckets[b].append(name)
        totals["tweens"] += len(tw)
        totals["manual"] += manual
        totals["frames25ok"] += frames[(25, True)]
        totals["frames30ok"] += frames[(30, True)]
        totals["framesN"] += frames[(25, True)] + frames[(25, False)]
        totals["frames50ok"] += frames[(50, True)]
        totals["frames60ok"] += frames[(60, True)]
        # Manual cascades that collapse into one stagger: same timeline, props,
        # duration and ease on sibling text units, starts in arithmetic steps.
        groups = collections.defaultdict(list)
        for t in tw:
            tgt = t.get("target") or {}
            if tgt.get("leafText") and tgt.get("sameClassSiblings", 0) > 2 and not t.get("stagger") and t["dur"] > 0:
                groups[(t["tl"], tuple(sorted(t["props"])), round(t["dur"], 6), t["ease"], tgt.get("sameClassSiblings"), t["from"])].append(t["start"])
        for g in groups.values():
            st = sorted(g)
            if len(st) < 3:
                totals["manual_other"] += len(st)
                continue
            d = [round(b - a, 6) for a, b in zip(st, st[1:])]
            if len(set(d)) == 1:
                totals["manual_collapsible"] += len(st)
                totals["manual_groups"] += 1
            else:
                totals["manual_other"] += len(st)

    if detail:
        for row in per:
            if detail in row["name"]:
                print(json.dumps(row, ensure_ascii=False, indent=1))
        return

    n = len(recs)
    print(f"compositions : {n}")
    for b in sorted(buckets):
        print(f"  {b:55s} {len(buckets[b]):4d}")
    print(f"\ntweens : {totals['tweens']}  propriétés animées : {sum(pc.values())}")
    for k, v in pc.most_common():
        print(f"  {k:16s} {v:6d}  {100*v/sum(pc.values()):5.1f} %")
    print("\npropriétés absentes du format (top 25) :")
    for k, v in absent.most_common(25):
        print(f"  {k:28s} {v}")
    print("\ncourbes (tweens de durée > 0) :")
    tot = sum(eases.values())
    for k, v in eases.most_common():
        print(f"  {k:8s} {v:6d}  {100*v/tot:5.1f} %")
    print("  noms les plus fréquents :", ", ".join(f"{k} ({v})" for k, v in ease_names.most_common(14)))
    print(f"\nbornes de tween tombant sur une image : 25 i/s {100*totals['frames25ok']/totals['framesN']:.1f} %, "
          f"30 i/s {100*totals['frames30ok']/totals['framesN']:.1f} %")
    print(f"   50 i/s {100*totals['frames50ok']/totals['framesN']:.1f} %, 60 i/s {100*totals['frames60ok']/totals['framesN']:.1f} %")
    print(f"cascades manuelles : {totals['manual_collapsible']} tweens réductibles en {totals['manual_groups']} stagger réguliers ; {totals['manual_other']} irréguliers")
    print(f"stagger GSAP par origine : {dict(stag)}")
    print(f"tweens sur un mot/caractère frère sans stagger (cascade écrite à la main) : {totals['manual']}")
    a = [r for r in per if not r["painted"]]
    print(f"\nnon peintes : {len(a)} ; dont avec transformOrigin : {sum(1 for r in a if r['origin'])}"
          f" ; avec cascade manuelle : {sum(1 for r in a if r['manual'])}")
    json.dump(per, open(path.replace(".jsonl", ".per.json"), "w"), ensure_ascii=False, indent=0)
    json.dump({k: v for k, v in buckets.items()}, open(path.replace(".jsonl", ".buckets.json"), "w"), ensure_ascii=False, indent=0)


main()
