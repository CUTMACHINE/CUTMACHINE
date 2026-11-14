#!/usr/bin/env python3
"""Planche d'images numérotées tirées d'un rendu, pour juger le calage.

    python3 planche.py rendu.mp4 planche.jpg 25 26 27 90 94 95 [--colonnes 8] [--largeur 240]

Chaque vignette porte le numéro de SON image dans la vidéo (pas son rang
dans la planche) : c'est ce qu'on compare aux coupes et aux temps. Les
images sont extraites par numéro exact (filtre select), jamais par un
temps en secondes arrondi. Seule dépendance : ffmpeg et ffprobe.
"""
import argparse
import os
import subprocess
import sys
import tempfile

POLICES = ["/System/Library/Fonts/Supplemental/Arial.ttf",
           "/System/Library/Fonts/Helvetica.ttc",
           "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"]


def sonde(video):
    sortie = subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
         "stream=width,height,r_frame_rate", "-of", "csv=p=0", video],
        capture_output=True, text=True, check=True).stdout.strip().split(",")
    largeur, hauteur = int(sortie[0]), int(sortie[1])
    num, den = sortie[2].split("/")
    return largeur, hauteur, int(num) / int(den)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("video")
    p.add_argument("sortie")
    p.add_argument("images", nargs="+", type=int)
    p.add_argument("--colonnes", type=int, default=8)
    p.add_argument("--largeur", type=int, default=240, help="largeur d'une vignette en pixels")
    a = p.parse_args()

    largeur, hauteur, _ = sonde(a.video)
    vl = a.largeur - a.largeur % 2
    vh = round(vl * hauteur / largeur / 2) * 2
    police = next((f for f in POLICES if os.path.exists(f)), None)
    with tempfile.TemporaryDirectory() as tmp:
        vignettes = []
        for n in a.images:
            chemin = os.path.join(tmp, "%06d.png" % n)
            filtre = "select=eq(n\\,%d),scale=%d:%d" % (n, vl, vh)
            if police:
                filtre += (",drawbox=x=0:y=0:w=%d:h=%d:color=black@0.7:t=fill,"
                           "drawtext=fontfile=%s:text=%d:x=4:y=3:fontsize=%d:fontcolor=0xFF5050"
                           % (vl // 4, vh // 12, police, n, max(12, vh // 16)))
            subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", a.video, "-vf", filtre,
                            "-frames:v", "1", "-vsync", "0", chemin], check=True)
            if not os.path.exists(chemin):
                sys.exit("image %d absente du rendu (au-delà de la fin ?)" % n)
            vignettes.append(chemin)
        colonnes = min(a.colonnes, len(vignettes))
        lignes = (len(vignettes) + colonnes - 1) // colonnes
        liste = os.path.join(tmp, "liste.txt")
        with open(liste, "w") as f:
            f.writelines("file '%s'\n" % v for v in vignettes)
        subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "concat", "-safe", "0", "-i", liste,
                        "-vf", "tile=%dx%d:padding=4:color=0x202020" % (colonnes, lignes),
                        "-frames:v", "1", a.sortie], check=True)
    print("%s : %d images" % (a.sortie, len(vignettes)))


if __name__ == "__main__":
    main()
