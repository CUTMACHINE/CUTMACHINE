# Sous-titres

## Sommaire

- [Dans le moteur : `generate_captions`](#dans-le-moteur--generate_captions)
- [À la main, pour un renvoi dans Resolve](#à-la-main-pour-un-renvoi-dans-resolve)
- [Relire les cartons comme un texte publié](#relire-les-cartons-comme-un-texte-publié)
- [Le dictionnaire](#le-dictionnaire)
- [La cadence : 18 CPS en moyenne](#la-cadence--18-cps-en-moyenne)
- [Découper par unité syntaxique](#découper-par-unité-syntaxique)
- [Caler les frontières sur les cuts](#caler-les-frontières-sur-les-cuts)
- [Safe zone : une ligne dans le moteur, deux au-delà de 650 px dans Resolve](#safe-zone--une-ligne-dans-le-moteur-deux-au-delà-de-650-px-dans-resolve)

Pour poser ou reprendre des cartons dans une timeline Resolve (ordre de
construction, style, timeline déjà retouchée), voir aussi
[resolve.md](resolve.md).

## Dans le moteur : `generate_captions`

Laisse `max_characters` à son défaut, sauf si la consigne donne un autre
nombre.

**Pour un livrable, découpe toi-même, avant de voir la découpe du moteur** :
à l'aveugle, l'auteur a préféré celle de l'agent 29 fois sur 35 (une série, à
confirmer). Corriger la proposition du moteur en la voyant en garde les
défauts. Lis les mots du montage (`get_timeline_transcript`), découpe par
unité syntaxique, passe tes `cartons` à `generate_captions` en `preview`,
puis sans ; écris dans le compte rendu que tu as découpé toi-même. Compte
environ 35 000 tokens par film d'une minute ; la découpe du moteur reste le
brouillon rapide.

N'installe pas de modèle de découpe toi-même (`train_caption_breaks`) : il
change les cartons de tous les films suivants, c'est à l'auteur de le décider.
Écris `break_model` dans le compte rendu.

Pour un film vertical, un style peut suivre la voix mot à mot
(`update_caption_style {word_animation: "highlight"}` ou `"reveal"`,
`highlight_color`). Ne le règle que si la consigne ou la charte le demande :
c'est un choix de goût. Seuls les cartons de `generate_captions` s'animent ;
un carton dont tu changes le nombre de mots (`set_clip_caption`) perd ses
temps et s'affiche entier, relance alors `generate_captions`.

Un mot de `cut_but_heard` s'ajoute à la main au carton voisin
(`set_clip_caption`), après contrôle du raccord. Relis aussi les mots
écartés (`unheard_words`) : un mot court peut manquer aux deux lectures et
avoir été dit. Une liste vide ne prouve rien si `montage_readings` vaut 0 :
fais d'abord les deux lectures du son monté.

## À la main, pour un renvoi dans Resolve

- **Rattache chaque mot au segment choisi, pas au plan** : garde tout mot
  qui chevauche le segment d'une image, et cale sur le bord de plan le plus
  proche celui qui tombe dans un creux.
- **Un carton tient jusqu'au suivant**, jamais jusqu'à la fin de son dernier
  mot.
- Les mots des passages ajoutés par le monteur se projettent sur les plans
  **relevés dans Resolve**, pas sur le montage CUTMACHINE, qui ignore ses
  retouches.

## Relire les cartons comme un texte publié

- Pour des cartons faits ou retouchés hors du moteur (Resolve), fais le
  diff mot à mot contre les deux lectures de `transcribe_timeline` ; un
  « donc » que seule la lecture normale efface est un tic, pas un manque.
- **whisper écorche les noms propres** et le vocabulaire de métier
  (« carte en bord » pour storyboard) : relis-les.
- La dernière cue tombe sur la durée du film, à l'image près.

## Le dictionnaire

Les graphies imposées par le monteur vivent dans
[`docs/DICTIONNAIRE_GRAPHIES.md`](../../../docs/DICTIONNAIRE_GRAPHIES.md),
que seul le dépôt porte (`get_procedure` ne la sert pas) : **passe chaque
carton contre cette table avant de livrer**, dans le moteur comme dans
Resolve.

Dans Resolve, lis le texte par `GetName()`, pas dans le `.drt` : un carton
retapé par le monteur garde son ancien texte dans `<Name>`, le nouveau vit dans
son `<EffectFiltersBA>` (vu le 29 septembre 2026 : la graphie corrigée à
l'écran et par l'API, l'ancienne dans `<Name>`).

## La cadence : 18 CPS en moyenne

**Consigne du monteur : 18 caractères par seconde en moyenne sur la
séquence**, pas par carton : publie la moyenne pondérée
(`somme(caractères) / somme(durées affichées)`) et la distribution. Les
repères du métier (20 CPS au plus, 0,83 s au moins) : le réglage maison en
sort des deux côtés, par choix, et il faut le dire. Sous 0,83 s, c'est le
réglage : **ne refusionne pas les cartons courts**.

## Découper par unité syntaxique

Un carton porte un groupe grammatical entier : jamais de coupe après un
déterminant, une préposition, un pronom sujet, un auxiliaire, une négation,
un adverbe ou une conjonction, ni par-dessus une fin de phrase. Coupe de
préférence sur une fin de phrase, une virgule, avant une conjonction, un
relatif ou un groupe prépositionnel.

L'auteur a tranché (REGLES-2026-10) : jamais d'article en fin de carton ; le nom
garde son adjectif, une locution reste entière ; pas de carton qui
clignote ; quand un groupe doit être coupé, la coupe qui équilibre les
durées gagne.

## Caler les frontières sur les cuts

**Consigne du monteur : les frontières de carton se calent sur les cuts**,
plans de coupe et incrustations compris. `generate_captions` le fait ; à la
main (Resolve), sur le relevé de toutes les pistes vidéo, une frontière à
moins de 12 images d'un cut s'y pose, sauf si un carton tombe sous 10 images
ou si le déplacement découvre un mot (DEC18).

- FR et EN se calent chacun de leur côté : les cartons anglais ne partagent pas
  toujours les frontières françaises.
- Le calage est la dernière passe, à refaire après chaque retouche du
  monteur : dans un SRT, les deux horodatages ; dans le `.drt`, le
  `<Start>` du carton et la `<Duration>` du précédent.

## Safe zone : une ligne dans le moteur, deux au-delà de 650 px dans Resolve

Un carton du moteur tient sur une ligne : un carton trop large
(`over_width`) se recoupe. Dans Resolve, **consigne du monteur : un carton
de plus de 650 px de large (≈ 60 % de 1080) passe sur deux lignes** ; la
largeur tranche, pas le compte de caractères (l'anglais se mesure dans la
graisse de sa piste). Pour la mesurer, rends le
carton dans la police et à la taille de sa piste (PIL).

Pour rendre une image de contrôle depuis Resolve : `SetRenderSettings` avec
`MarkIn` = `MarkOut`, `"ExportSubtitle": True, "SubtitleFormat": "BurnIn"`,
format TIFF ; désactive la piste de l'autre langue le temps du rendu, puis
**remets le format de rendu d'origine et supprime tes jobs** : c'est le projet du
monteur.

Deux lignes équilibrées, la plus longue en bas, jamais après un mot-outil ;
un carton qui n'a qu'une coupe légale l'accepte, même déséquilibrée.

Comment l'écrire :

- dans un SRT, un saut de ligne dans la cue ;
- dans le `.drt`, `ligne 1<br>ligne 2` dans `<Name>` (échappé `&lt;br&gt;`),
  exactement ce que Resolve écrit pour une cue de SRT sur deux lignes ; l'anglais
  donne `<b>ligne 1<br>ligne 2</b>`. L'API le relit avec U+2028 ;
- **ne touche pas aux cartons que le monteur a mis en forme à la main** : ils
  portent un `<EffectFiltersBA>` non vide (texte riche compressé en zstd, avec
  ses propres coupes). Seuls les `<EffectFiltersBA/>` vides sont à toi.
