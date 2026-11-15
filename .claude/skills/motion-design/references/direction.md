# Direction — couleurs, typographie, lecture, son, livraison

Ce qui se décide avant le premier keyframe, et la liste de contrôle qui
passe avant toute livraison.

## Couleurs

Sans charte : une base, une couleur forte sur un seul élément par image, un
halo au plus.

**Flat design** : aplats purs, deux ou trois couleurs, ni dégradé ni
texture ni ombre ; les formes et le texte bougent, pas la lumière.

## Typographie

- **Titre** : une police d'affichage, jamais celle du système ; graisse de
  la charte, sinon 600 à 800.
- L'interligne, et le corps d'un format à l'autre, dépendent de la
  direction artistique du film.
- **L'échelle de corps** est une table du générateur, qui refuse un autre
  corps ; une ligne qui ne tient pas se remet en page avant de sortir de
  l'échelle ; une exception s'écrit avec sa raison.
- **Un mot fort par titre** : couleur, soulignement qui se trace, trait de
  surligneur qui avance (`highlights`, voir [motifs.md](motifs.md),
  motif 3), ou cartouche qui arrive derrière lui quelques images après le
  mot.
- Un visage ne se couvre pas : sur un plan serré, le texte descend sous le
  menton ou s'écarte, quitte à décentrer ce que la charte veut centré.

## Temps de lecture

Planchers de sous-titrage (0,83 s chez Netflix, 0,35 s par mot à la BBC,
15 caractères par seconde pour le CSA belge) ; un texte qui porte le
message (accroche, promesse, signature) se compte deux fois. Dans le
générateur, la durée se calcule, et le script refuse un carton trop court :

```python
def lecture(texte, fps, fois=1):
    """Frames a text must stay whole and legible (fois=2: the message)."""
    s = max(0.83, 0.35 * len(texte.split()), len(texte) / 15)
    return math.ceil(s * fois * fps)

assert debut_sortie - fin_entree >= lecture(texte, FPS), texte
```

L'entrée et la sortie ne se comptent pas, ni un volet à mi-course ou un
flou qui se résout. **Une image à comparer, ou un texte posé sur une image
qui bouge, se lit plus lentement** : prends au moins la lecture comptée
deux fois.

## Son

Le son fait la moitié de la qualité perçue ; ne livre pas muet sans qu'on
l'ait demandé.

- Un bruitage marque un geste qui compte, deux à trois images avant
  l'image ; posé sur chaque carton, il devient systématique.
- Un changement d'écran prend un whoosh réel, différent à chaque coupe,
  son pic sur la coupe ; un son généré s'y entend comme un bip de
  marqueur.
- Une musique en boucle n'a pas de fin, et un fondu n'en est pas une : cale
  le retour de sa boucle sur le coup final et laisse-le résonner.
- Lit musical bas (gain 0,2 à 0,3), plus bas sous une voix (≈ 0,126,
  −18 dB).
- Les bruitages viennent des bibliothèques de la personne (demande où) ;
  un son généré est un dernier recours, et se dit.
- Tu n'écoutes pas : mesure chaque son contre la musique et livre la sonie
  mesurée (`loudness` de `render_motion_video`).

## Liste de contrôle avant livraison

- [ ] Toutes les versions et tous les formats rendus, chacun planché à ses
      moments clés (textes plus longs d'une version : lignes qui débordent ?).
