"""Écrit les compositions d'exemple du skill motion-design.

    python3 generer_exemples.py ../exemples

Les exemples sont neutres (ni texte, ni marque, ni police de commanditaire)
et utilisent la Fira Sans Condensed de tests/fixtures/fonts. Après une
modification : recharger chaque fichier dans le moteur, rendre, planche.
"""
import json, os, sys

OUT = sys.argv[1]
os.makedirs(OUT, exist_ok=True)

F = "Fira Sans Condensed"
OUT_E = "cubic-bezier(0.16, 1, 0.3, 1)"     # entrances
IN_E = "cubic-bezier(0.7, 0, 0.84, 0)"      # exits
MOVE = "cubic-bezier(0.65, 0, 0.35, 1)"     # moves
ACCENT = "#FFD23F"
FOND = "#0A0A0F"
TEXTE = "#F4F4F5"


def ip(frames, values, easing=OUT_E):
    return {"interpolate": {"frames": frames, "values": values, "easing": easing}}


def spring(frm, to, delay=0, damping=20, stiffness=90, mass=1):
    return {"spring": {"from": frm, "to": to, "delay": delay, "damping": damping,
                       "stiffness": stiffness, "mass": mass}}


def text(content, size, weight=600, color=TEXTE, **style):
    s = {"fontFamily": F, "fontWeight": weight, "fontSize": size, "lineHeight": 1.0, "color": color,
         "flexShrink": 0}
    s.update(style)
    return {"type": "text", "content": content, "style": s}


def cartouche(children, size, **style):
    pv, ph = round(size * 0.14), round(size * 0.28)
    s = {"backgroundColor": ACCENT, "overflow": "hidden", "flexDirection": "column",
         "paddingTop": pv, "paddingBottom": pv, "paddingLeft": ph, "paddingRight": ph}
    s.update(style)
    return {"type": "box", "style": s, "children": children}


def comp(duration, root, props=None, **extra):
    c = {"width": 1080, "height": 1920, "fps": 25, "duration": duration}
    if props:
        c["props"] = props
    c.update(extra)
    c["root"] = root
    return c


def write(name, c):
    with open(os.path.join(OUT, name), "w") as f:
        json.dump(c, f, ensure_ascii=False, indent=1)
        f.write("\n")
    print("écrit", name)


# 1. Three-property entrance, word cascade, highlighted word, faster exit.
titre = text({"prop": "titre"}, 118, textAlign="center", letterSpacing=-3)
titre.update({"units": "word", "stagger": {"each": 3},
              "ranges": [{"unit": "word", "start": {"prop": "mot_fort"}, "count": 1,
                          "style": {"color": {"prop": "accent"}}}],
              "animate": {"opacity": ip([3, 9], [0, 1]),
                          "translateY": spring(30, 0, delay=3, damping=14, stiffness=160, mass=0.6)}})
pastille = {"type": "box", "style": {"backgroundColor": "#1C1C26", "padding": "18px 40px", "borderRadius": 999},
            "animate": {"opacity": ip([18, 26], [0, 1]),
                        "translateY": spring(40, 0, delay=18),
                        "scale": spring(0.94, 1, delay=18)},
            "children": [text("Sous-titre en pastille", 44, weight=500, color="#A1A1AA")]}
write("entree-cascade-sortie.json", comp(75, {
    "type": "fill", "style": {"backgroundColor": FOND, "justifyContent": "center", "alignItems": "center"},
    "children": [{"type": "box", "id": "sortie",
                  "style": {"flexDirection": "column", "alignItems": "center", "gap": 40, "width": "84%"},
                  "animate": {"translateY": ip([63, 72], [0, -42], IN_E), "opacity": ip([63, 72], [1, 0], IN_E)},
                  "children": [titre, pastille]}]},
    props={"titre": {"type": "text", "default": "Trois mots qui comptent vraiment"},
           "accent": {"type": "color", "default": ACCENT},
           "mot_fort": {"type": "number", "default": 3, "min": 0, "max": 20}}))

# 2. Kinetic, slot: one word per beat; each card pushes the previous one
# out of a fixed slot; the whole sentence closes the run.
SLOT = 300
mots = [("UN", 2), ("MOT", 9), ("PAR", 17), ("TEMPS", 24)]
fin, sortie = 32, 50
temps = [t for _, t in mots] + [fin]
items = []
for i, (m, t) in enumerate(mots):
    tn = temps[i + 1]
    c = cartouche([text(m, 220, color="#000000")], 220, position="absolute", left=0, top=0)
    c["animate"] = {"translateY": ip([t - 1, t + 4, tn - 1, tn + 4], [SLOT, 0, 0, -SLOT])}
    items.append(c)
c = cartouche([text("UN MOT\nPAR TEMPS", 110, color="#000000")], 110, position="absolute", left=0, top=0)
c["animate"] = {"translateY": ip([fin - 1, fin + 4, sortie, sortie + 5], [SLOT, 0, 0, -SLOT])}
items.append(c)
write("kinetic-fente.json", comp(60, {"type": "fill", "style": {"backgroundColor": FOND}, "children": [
    {"type": "box", "style": {"position": "absolute", "left": 108, "top": 800, "width": 864, "height": SLOT,
                              "overflow": "hidden"}, "children": items}]}))

# 3. Kinetic, actor card: enters from one side with the first line; each
# next line slides in while the card grows; it leaves by the other side.
size = 118
pv = round(size * 0.14)
lignes = [("UNE FORME", 2), ("QUI PORTE", 9), ("LES MOTS", 17)]
kids = [text(lignes[0][0], size, color="#000000")]
hf, hv = [], []
for n, (s, t) in enumerate(lignes[1:], start=2):
    node = text(s, size, color="#000000")
    node["animate"] = {"translateX": ip([t - 1, t + 5], [900, 0])}
    kids.append(node)
    hf += [t - 1, t + 5]
    hv += [2 * pv + (n - 1) * size, 2 * pv + n * size]
carte = cartouche(kids, size, height=2 * pv + size)
carte["animate"] = {"height": ip(hf, hv), "translateX": ip([0, 7], [-1100, 0])}
write("kinetic-cartouche.json", comp(60, {"type": "fill", "style": {"backgroundColor": FOND}, "children": [
    {"type": "box", "style": {"position": "absolute", "left": 108, "top": 800}, "children": [carte],
     "animate": {"translateX": ip([50, 58], [0, 1100], IN_E)}}]}))


# 4. Kinetic, marquee: two full-width bands enter from opposite sides; the
# text runs, slows and locks; they cross on the way out.
def bande(top, phrase, size, t_in, cote):
    pvb = round(size * 0.14)
    rang = {"type": "box", "style": {"width": 4200, "flexShrink": 0, "flexDirection": "row",
                                     "justifyContent": "center"},
            "children": [text(" · ".join([phrase] * 3), size, color="#000000")],
            "animate": {"translateX": ip([t_in - 1, t_in + 22], [-cote * 700, 0])}}
    bandeau = {"type": "box", "style": {"width": 1080, "height": size + 2 * pvb, "backgroundColor": ACCENT,
                                        "overflow": "hidden", "flexDirection": "row",
                                        "justifyContent": "center", "alignItems": "center"},
               "children": [rang], "animate": {"translateX": ip([t_in - 1, t_in + 7], [cote * 1080, 0])}}
    return {"type": "box", "style": {"position": "absolute", "left": 0, "top": top}, "children": [bandeau],
            "animate": {"translateX": ip([50, 59], [0, -cote * 1080], IN_E)}}


write("kinetic-bandeau.json", comp(60, {"type": "fill", "style": {"backgroundColor": FOND}, "children": [
    bande(800, "LE TEXTE DÉFILE", 132, 2, -1),
    bande(800 + 132 + 2 * round(132 * 0.14) + 20, "ET SE CALE SUR LE TEMPS", 104, 9, 1)]}))

# 5. Kinetic, trapdoor: each line rises from below the frame in its card on
# its beat and settles on a spring, as a staircase; they drop back down.
items = []
for i, (s, t, dx) in enumerate([("CHAQUE", 2, 0), ("LIGNE", 9, 54), ("MONTE", 17, 0), ("À SON TOUR", 24, 108)]):
    c = cartouche([text(s, 118, color="#000000")], 118)
    c["animate"] = {"translateY": spring(1100, 0, delay=t - 1, damping=21, stiffness=190)}
    items.append({"type": "box", "style": {"alignSelf": "flex-start", "marginLeft": dx}, "children": [c],
                  "animate": {"translateY": ip([50 + i, 56 + i], [0, 1100], IN_E)}})
write("kinetic-trappe.json", comp(62, {"type": "fill", "style": {"backgroundColor": FOND}, "children": [
    {"type": "box", "style": {"position": "absolute", "left": 108, "top": 760, "flexDirection": "column",
                              "alignItems": "flex-start", "gap": 14}, "children": items}]}))

# 6. Full frame in five layers: drifting mesh background, media (a gradient
# standing in for a photo) with Ken Burns, text, grade, grain and vignette;
# motion blur on the whole composition.
def tache(size, top, left, color, dx, blur):
    return {"type": "box", "style": {"position": "absolute", "top": top, "left": left, "width": size,
                                     "height": size, "borderRadius": size,
                                     "backgroundImage": "radial-gradient(circle, %s 0%%, transparent 62%%)" % color,
                                     "filter": "blur(%dpx)" % blur},
            "animate": {"translateX": ip([0, 75], [0, dx], MOVE)}}


photo = {"type": "box", "style": {"width": 860, "height": 860, "borderRadius": 32, "overflow": "hidden"},
         "animate": {"opacity": ip([0, 10], [0, 1]), "translateY": spring(40, 0), "scale": spring(0.94, 1)},
         "children": [{"type": "fill", "style": {
             "backgroundImage": "linear-gradient(135deg, #E4572E 0%, #3D5AFE 100%)"},
             "animate": {"scale": ip([0, 75], [1, 1.08], MOVE), "translateX": ip([0, 75], [0, -25], MOVE)}}]}
titre6 = text("Cinq couches", 130, textAlign="center", letterSpacing=-4)
titre6.update({"units": "word", "stagger": {"each": 4},
               "animate": {"opacity": ip([12, 18], [0, 1]), "translateY": spring(30, 0, delay=12, damping=14,
                                                                              stiffness=160, mass=0.6)}})
write("plein-cadre.json", comp(75, {"type": "fill", "children": [
    {"type": "fill", "style": {"backgroundColor": FOND}, "children": [
        tache(1400, -550, -350, "#7C3AED33", 50, 60), tache(1100, 1300, 300, "#22D3EE22", -40, 80)]},
    {"type": "fill", "style": {"justifyContent": "center", "alignItems": "center", "gap": 70,
                               "flexDirection": "column"}, "children": [photo, titre6]},
    {"type": "fill", "style": {"backgroundColor": "#7C3AED", "mixBlendMode": "soft-light", "opacity": 0.18}},
    {"type": "fill", "style": {"backgroundImage":
                               "radial-gradient(ellipse at 50% 50%, transparent 56%, rgba(0, 0, 0, 0.22) 100%)"}}]},
    grain={"amount": 0.05, "size": 2, "seed": 3}, motionBlur={"samples": 6, "shutterAngle": 180}))

# 7. Rolling counter: text cannot interpolate a number, so each digit is a
# 0-9 column (two turns) scrolling in a window to its digit; the rolls
# stop in cascade.
H = 220
colonne = "\n".join(str(d % 10) for d in range(20))
rouleaux = []
for i, d in enumerate([1, 2, 8]):
    cible = 10 + d
    fin_r = 30 + i * 5
    rouleaux.append({"type": "box", "style": {"height": H, "overflow": "hidden"}, "children": [
        {"type": "text", "content": colonne,
         "style": {"fontFamily": F, "fontWeight": 600, "fontSize": H, "lineHeight": 1.0, "color": TEXTE,
                   "textAlign": "center", "flexShrink": 0},
         "animate": {"translateY": ip([4, fin_r], [0, -cible * H])}}]})
# A box is a row by default (as in CSS): push to the bottom in a column.
rouleaux.append({"type": "box", "style": {"height": H, "flexDirection": "column", "justifyContent": "flex-end",
                                          "paddingBottom": round(H * 0.06)}, "children": [
    text("%", round(H * 0.6), color=ACCENT)]})
write("compteur.json", comp(60, {"type": "fill", "style": {"backgroundColor": FOND, "justifyContent": "center",
                                                         "alignItems": "center"}, "children": [
    {"type": "box", "style": {"flexDirection": "row", "alignItems": "flex-end", "gap": 6}, "children": rouleaux}]}))


# 8. Transitions: a series slides then fades its scenes; the last one
# arrives as a hand-made whip (translation and blur settling).
def scene(fond, mot, **kw):
    n = {"type": "sequence", "style": {"backgroundColor": fond, "justifyContent": "center", "alignItems": "center"},
         "children": [text(mot, 160, textAlign="center")]}
    n.update(kw)
    return n


fouet = {"type": "sequence", "style": {"backgroundColor": "#1C1C26", "justifyContent": "center",
                                       "alignItems": "center"},
         "children": [{"type": "box", "children": [text("FOUETTÉ", 160)],
                       "animate": {"translateX": ip([0, 7], [1300, 0]),
                                   "filter": ip([0, 7], ["blur(12px)", "blur(0px)"])}}]}
write("transitions.json", comp(96, {"type": "series", "children": [
    scene("#E4572E", "GLISSÉ", duration=36),
    {"type": "transition", "presentation": "slide", "direction": "from-right",
     "timing": {"duration": 10, "easing": MOVE}},
    scene("#3D5AFE", "FONDU", duration=36),
    {"type": "transition", "presentation": "fade", "timing": {"duration": 8}},
    fouet]}))

# 9. Window and text-as-image: a flat colour with a window opening onto the
# picture (mask-out), then a giant word showing the picture through its
# letters (mask-in) while it settles back.
image = {"type": "fill", "style": {"backgroundImage": "linear-gradient(135deg, #E4572E 0%, #3D5AFE 100%)"}}
fen = {"type": "sequence", "from": 0, "duration": 40, "children": [
    image,
    {"type": "fill", "style": {"isolation": "isolate"}, "children": [
        {"type": "fill", "style": {"backgroundColor": ACCENT}},
        {"type": "fill", "style": {"backgroundColor": "#000000", "mixBlendMode": "mask-out"},
         "animate": {"clipPath": ip([2, 14], ["inset(50% 50% 50% 50%)", "inset(12% 10% 30% 10%)"])}}]}]}
mot_image = {"type": "sequence", "from": 40, "duration": 40, "style": {"backgroundColor": FOND}, "children": [
    {"type": "fill", "style": {"isolation": "isolate", "justifyContent": "center", "alignItems": "center"},
     "children": [image,
                  {"type": "box", "style": {"mixBlendMode": "mask-in"},
                   "animate": {"scale": ip([0, 20], [1.6, 1])},
                   "children": [text("IMAGE", 330, weight=600, textAlign="center")]}]}]}
write("fenetre-et-texte-image.json", comp(80, {"type": "fill", "style": {"backgroundColor": FOND},
                                              "children": [fen, mot_image]}))

# 10. Formats x versions. A prop is set by formats OR by versions; when a
# value depends on both (here the line breaks), versions give both texts and
# the format picks the node through a `fmt` prop.
def bloc(content_prop, size, fmt):
    t = text({"prop": content_prop}, size, textAlign="center")
    t.update({"units": "line", "stagger": {"each": 3},
              "animate": {"opacity": ip([2, 8], [0, 1]), "translateY": spring(30, 0, delay=2, damping=14,
                                                                             stiffness=160, mass=0.6)}})
    return {"type": "fill", "visible": {"prop": "fmt", "map": {"9x16": fmt == "9x16", "16x9": fmt == "16x9"}},
            "style": {"justifyContent": "center", "alignItems": "center"}, "children": [t]}


write("declinaisons.json", comp(50, {"type": "fill", "style": {"backgroundColor": FOND}, "children": [
    bloc("titre_court", 120, "9x16"), bloc("titre_long", 110, "16x9")]},
    props={"fmt": {"type": "enum", "values": ["9x16", "16x9"], "default": "9x16"},
           "titre_court": {"type": "text", "default": "UNE DATE\nÀ RETENIR"},
           "titre_long": {"type": "text", "default": "UNE DATE À RETENIR"}},
    formats={"9x16": {"width": 1080, "height": 1920, "props": {"fmt": "9x16"}},
             "16x9": {"width": 1920, "height": 1080, "props": {"fmt": "16x9"}}},
    variants={"mars": {"props": {"titre_court": "SAMEDI\n9 MARS", "titre_long": "SAMEDI 9 MARS"}},
              "avril": {"props": {"titre_court": "SAMEDI\n27 AVRIL", "titre_long": "SAMEDI 27 AVRIL"}}}))

# 11. Kinetic warp: After's Mesh Warp (mesh()), a 2 x 4 grid whose middle
# lines move whole; the outer columns stay put and bound the stretch to the
# word. The curve is the tutorial's, measured on its render.
def grille(x, y):
    xs = ["0", "25%", "%s%%" % x, "75%", "100%"]
    ys = ["0", "%s%%" % y, "100%"]
    return "mesh(2 4, " + ", ".join("%s %s" % (a, b) for b in ys for a in xs) + ")"


cles = [(0, 50, 50), (17, 65, 50), (33, 65, 62), (50, 36, 62), (67, 36, 40), (83, 50, 50)]
write("kinetic-warp.json", comp(100, {"type": "fill", "style": {"backgroundColor": FOND}, "children": [
    {"type": "fill", "style": {"justifyContent": "center", "alignItems": "center", "filter": grille(50, 50)},
     "animate": {"filter": {"interpolate": {"frames": [c[0] for c in cles],
                                            "values": [grille(c[1], c[2]) for c in cles],
                                            "easing": "cubic-bezier(0.325, 0, 0, 1)"}}},
     "children": [text("MOT\nCLÉ", 330, lineHeight=0.95, textAlign="left")]}]}))
