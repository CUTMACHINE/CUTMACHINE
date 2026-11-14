# Banc de transcription — ce que les modèles écrivent des hésitations

Date : 1er novembre 2026. Statut : **mesuré contre une transcription
verbatim faite à la main** (C-PROM, 70 minutes), en local, sur les mêmes
morceaux pour chaque modèle. Suite du
banc d'alignement, qui a montré qu'aucun
aligneur ne rattrape un mot que la transcription a manqué.

## Verdict

Aucun modèle mesuré n'écrit les « euh ». Le Whisper du moteur, en mode
verbatim, est celui qui en écrit le plus : 21 sur 365, grâce à la consigne
d'hésitations qu'il reçoit. Les autres n'en écrivent aucun. Deux
reconnaisseurs de phonèmes, qui transcrivent des sons et pas des mots, n'en
entendent pas davantage : un son de type « euh » sur 3 % et 14 % d'entre eux.
Tous ont appris sur du texte public, où les hésitations ne sont pas écrites,
et ils ont appris à les taire. **Changer de modèle de transcription
n'apportera pas les mots manqués.**

Un détecteur acoustique sans apprentissage (une voyelle tenue, stable en
hauteur et en timbre) fait mieux. Réglé sur les locuteurs belges et
suisses, mesuré sur les locuteurs français : là où la transcription du
moteur, alignée par MFA, n'explique pas ce qu'il entend, il retrouve 35 %
des « euh » qu'elle a sautés (42 sur 120), avec 0,8 fausse alerte par
minute. Le contrôle du son hors des mots du moteur (A18) en retrouve 9 %
(11 sur 120), avec 1,3 fausse alerte par minute. C'est une piste, pas
encore un outil : deux « euh » sautés sur trois restent introuvables.

Sur le reste du texte, Qwen3-ASR 1.7B fait environ 22 % d'erreurs de moins
que le Whisper du moteur (9,3 % contre 11,9 % hors hésitations) et garde un
peu plus de répétitions. La consigne de Whisper répétée à chaque fenêtre
(`carry_initial_prompt`, whisper.cpp 1.9.5, la piste d'ITW9) casse un
morceau sur six : à écarter telle quelle.

## Référence et protocole

[C-PROM](https://doi.org/10.26037/yareta:2jqreak2hrexdirkjfyp4gq2gq) : 24
enregistrements de locuteurs de Belgique, de France et de Suisse, sept
styles de parole, transcription verbatim alignée et vérifiée à la main
(CC BY-NC-SA : un jeu de test local, jamais committé). Les mêmes 139
morceaux de 60 s au plus que le banc d'alignement (`cprom.py prepare`),
12 553 mots dont 423 hésitations (365 « euh », des « ben », « mh »,
« hein »), 121 mots répétés à la suite (« le le ») et 15 mots interrompus
(« incon- »).

`cprom.py score-transcripts` passe la référence et chaque transcription par
la normalisation du texte donné à l'aligneur (`spoken` de `mfa_align.py`,
ex-`nemo_align.py` : casse,
ponctuation, chiffres lus), replie les graphies d'une hésitation (« heu » =
« euh », « hum » = « mh »), aligne les deux mot à mot et compte :

- le taux d'erreur, et le même sans aucune hésitation de part et d'autre
  (« hors hésitations ») : ce que mesurent les classements publics ;
- les hésitations, les « euh », les répétitions et les mots interrompus de
  la référence que la transcription écrit à leur place ;
- les hésitations que la transcription écrit là où la référence n'en a pas.

Vérifié sur la référence elle-même (0 % d'erreur, tout écrit) et sur la
référence privée de ses hésitations (3,4 % d'erreur, aucune hésitation, 0 %
hors hésitations).

## Résultats

| Modèle | Erreurs | Hors hésitations | « euh » écrits | Hésitations écrites | Répétitions gardées | Interrompus |
|---|---:|---:|---:|---:|---:|---:|
| Whisper large-v3, le moteur (whisper.cpp 1.7.4, verbatim) | 14,5 % | 11,9 % | 21 / 365 | 10,4 % | 40 / 121 | 1 / 15 |
| Whisper large-v3, whisper.cpp 1.9.5, consigne répétée | 28,6 % | 26,5 % | 20 / 365 | 9,5 % | 33 / 121 | 1 / 15 |
| Qwen3-ASR 1.7B | 12,3 % | 9,3 % | 0 / 365 | 2,4 % | 45 / 121 | 3 / 15 |
| Qwen3-ASR 1.7B, consigne d'hésitations | 12,1 % | 9,2 % | 0 / 365 | 2,6 % | 45 / 121 | 3 / 15 |
| NeMo `stt_fr_conformer_ctc_large` | 19,1 % | 16,4 % | 0 / 365 | 0,7 % | 30 / 121 | 0 / 15 |
| Parakeet TDT 0.6B v3 | 55,0 % | 53,5 % | 0 / 365 | 0,9 % | 8 / 121 | 0 / 15 |

Voxtral Mini 3B n'a passé que 32 morceaux (arrêté, plus bas). Sur ces 32
morceaux : 10,6 % d'erreurs hors hésitations, contre 9,3 % pour Qwen3-ASR et
11,7 % pour le moteur ; aucun « euh » sur 113, comme les deux autres sur ce
même sous-ensemble ; 8 répétitions sur 41, contre 16 et 13.

Sur la parole spontanée seule (interview, itinéraire, récit de vie), le
moteur écrit 21 « euh » sur 252 (8,3 %) ; ses 21 « euh » en viennent tous.

### Ce que chacun a fait

- **Le moteur** : la consigne « Euh, heu, hum, mmh… » lui fait écrire
  quelques hésitations, et il en invente 9 où la référence n'en a pas.
- **whisper.cpp 1.9.5 avec `--carry-initial-prompt`** : sur 22 morceaux sur
  139, la transcription se réduit à « ... ... ». Whisper recopie les points
  de suspension de la consigne au lieu de transcrire. Ailleurs, rien de
  plus que le moteur. Cette option ne doit pas entrer dans le moteur avec
  cette consigne ; une consigne sans points de suspension reste à essayer.
- **Qwen3-ASR** : le texte le plus juste, propre comme un sous-titre ; la
  consigne ne change rien aux hésitations.
- **Parakeet v3** détecte la langue lui-même, sans qu'on puisse imposer le
  français : 65 morceaux sur 139 sortent en anglais, une sorte de traduction
  mot à mot (« You have perceived intuitively that there are some
  things… »).
- **Le FastConformer hybride français** en cache
  (`stt_fr_fastconformer_hybrid_large_pc`) ne se charge pas avec NeMo 3.0.0
  (`conv_pointwise_type` inconnu de `ConformerEncoder`) : pas mesuré.

### Les phonèmes

`phoneme_recognize.py` passe un reconnaisseur de phonèmes CTC sur les mêmes
morceaux ; `cprom.py score-phones` regarde ce qu'il émet à l'intérieur de
chaque « euh » de la référence.

| Reconnaisseur | Un son quelconque | Un son de type « euh » ([ø], [œ], [ə]) | Sons de type « euh » hors de tout mot |
|---|---:|---:|---:|
| WavLM-Large français (`imenba6/wavlm-large-fr-phoneme-recognition`) | 81 / 365 (22 %) | 9 / 365 (2,5 %) | 0 |
| wav2vec2 XLSR-53 espeak (`facebook/wav2vec2-xlsr-53-espeak-cv-ft`) | 133 / 365 (36 %) | 52 / 365 (14 %) | 0 |

Le premier « euh » de C-PROM (3,31 à 3,57 s) montre le défaut : le
reconnaisseur français passe du [ɛ] de « avait » au [d] de « des » sans
rien émettre entre les deux. Les deux ont appris sur Common Voice, de la
lecture transcrite sans hésitations.

### Un détecteur acoustique des pauses remplies

`filled_pauses.py` n'apprend rien. Un « euh », ou une voyelle tenue pendant
que le locuteur cherche son mot, garde sa hauteur et son timbre 200 ms ou
plus, là où la parole change toutes les quelques dizaines de millisecondes
(Goto, Itou et Hayamizu, 1999). Par trame de 10 ms : voisement et hauteur
par YIN (de Cheveigné et Kawahara, 2002), douze coefficients cepstraux, et
la ligne de voix du moteur ; une trame est stable quand toute une fenêtre
autour d'elle est voisée et forte, que sa hauteur tient en quelques
demi-tons et que son cepstre bouge peu ; une suite de trames stables assez
longue est une détection.

La cible : les 558 syllabes que C-PROM marque comme une hésitation (« z »
de sa tire `delivery`), 344 sur un « euh » écrit et 214 voyelles étirées
(« leeeee »), qu'un monteur coupe aussi. Les seuils sont cherchés sur les
locuteurs belges et suisses et les trois premiers itinéraires (41 minutes),
puis mesurés sur les locuteurs français et les quatre autres itinéraires
(28 minutes), qu'ils n'ont jamais vus (`cprom.py is_tuning_recording`).

Une première version mesurait la hauteur par autocorrélation : au cœur
des « euh », elle variait de 12 demi-tons en médiane, une octave, par sauts
d'octave. Avec YIN, 1,5 demi-ton dans un « euh » contre 6 dans un mot, et un
cepstre qui bouge d'un tiers de moins (6,3 contre 9,7).

| Seuils retenus (réglage) | Valeur |
|---|---:|
| Voisement minimal (1 − d′ de YIN) | 0,6 |
| Écart de hauteur dans la fenêtre | 2 demi-tons |
| Changement cepstral moyen | 30ᵉ centile des trames fortes |
| Fenêtre | 80 ms |
| Durée minimale | 200 ms |

| Détecteur seul, contre les syllabes d'hésitation | Réglage | Test |
|---|---:|---:|
| « euh » trouvés | 109 / 223 (49 %) | 62 / 121 (51 %) |
| Voyelles étirées trouvées | 70 / 151 (46 %) | 26 / 63 (41 %) |
| Détections sur aucune hésitation | 46 %, 3,9 par minute | 41 %, 2,4 par minute |

Seul, il est trop bavard : une voyelle finale allongée en fin de groupe,
que C-PROM ne marque pas comme une hésitation, lui ressemble.
`cprom.py score-dropped-hesitations` le met donc face à la transcription :
le texte du moteur (Whisper verbatim), aligné par MFA, et seulement les
« euh » que ce texte a sautés. Une détection est gardée quand les sons que
MFA y aligne, d'une durée ordinaire (moins de 300 ms, seuil choisi sur le
réglage), en couvrent moins de la moitié : elle tombe dans un trou, ou sur
un son que MFA a étiré pour absorber le « euh » dans un mot voisin.

| « euh » sautés par la transcription du moteur | Réglage | Test |
|---|---:|---:|
| « euh » sautés | 228 sur 234 | 120 sur 131 |
| Contrôle du son hors des mots (A18, règles du moteur) : retrouvés | 32 (14 %) | 11 (9 %) |
| Contrôle du moteur : alertes sur aucune hésitation | 101, 2,4 par minute | 36, 1,3 par minute |
| Détecteur, là où l'alignement ne l'explique pas : retrouvés | 99 (43 %) | 42 (35 %) |
| Détecteur : alertes sur aucune hésitation | 60, 1,5 par minute | 22, 0,8 par minute |

Le seuil de durée des sons arbitre entre les deux : à 180 ms, le test
retrouve 51 « euh » sur 120 pour 1,7 fausse alerte par minute ; à 300 ms, 42
pour 0,8. Gardé seulement hors des mots alignés (couvert à moins de moitié), le
détecteur ne retrouve plus que 23 « euh », mais avec 0,2 fausse alerte par
minute : MFA étire les voisins sur les « euh » sautés, comme Qwen
(banc d'alignement).

## Modèles et environnements

Tout en local, sous `build/` sur le disque du code, l'audio ne sort pas de
la machine. Téléchargements autorisés par l'auteur le 1er novembre 2026.

| Modèle | Version | Licence | Environnement |
|---|---|---|---|
| Whisper large-v3 | `ggml-large-v3.bin` ; moteur à c192195 ; whisper-cli 1.9.5 (branche `claude/whisper-1-9-5`) | MIT | moteur, `--transcribe --verbatim` |
| `stt_fr_conformer_ctc_large` | cache NeMo | CC-BY-4.0 | `build/nemo-env` (retiré, AL2), `nemo_transcribe.py` |
| `nvidia/parakeet-tdt-0.6b-v3` | `541d1f99`, `.nemo` SHA-256 `3cbdc858…d10d` | CC-BY-4.0 | `build/nemo-env` (retiré, AL2), `nemo_transcribe.py` |
| `Qwen/Qwen3-ASR-1.7B-hf` | `bcd2b5b7` | Apache 2.0 | `build/qwen-aligner-env` (transformers 5.19.0), `hf_transcribe.py`, GPU du Mac en bfloat16 |
| `mistralai/Voxtral-Mini-3B-2507` | `3060fe34` | Apache 2.0 | `build/voxtral-env` (torch 2.14.1, transformers 5.19.0, mistral-common 1.12.0), `hf_transcribe.py` |
| `imenba6/wavlm-large-fr-phoneme-recognition` | `dc60fc6e` | MIT | `build/qwen-aligner-env`, `phoneme_recognize.py`, CPU |
| `facebook/wav2vec2-xlsr-53-espeak-cv-ft` | `2c733782` | Apache 2.0 | `build/qwen-aligner-env`, `phoneme_recognize.py`, CPU |

Les modèles décodent sans échantillonnage (Whisper glouton comme le moteur,
`do_sample=False` pour Qwen3-ASR et Voxtral).

## Limites

- **C-PROM n'est pas un rush d'interview.** Lecture, radio, conférence,
  itinéraires et récits ; les rushs de l'auteur sont d'autres voix, d'autres
  micros, un autre débit. L'ordre des modèles peut
  y changer ; le constat sur les « euh » ne dépend pas du style (aucun
  modèle n'en écrit en lecture comme en récit).
- **Le découpage** en morceaux de 60 s au plus, dans les pauses de la
  référence, n'est pas celui du moteur, qui transcrit le rush entier par
  fenêtres de 30 s. Le moteur a reçu les morceaux comme des médias à part.
- **Voxtral arrêté** à 32 morceaux : en bfloat16 sur le GPU du Mac, il a
  saturé la mémoire (17 Go) en même temps que les autres passages, et le
  système a débordé sur le disque interne (15 Go d'échange). Un seul gros
  modèle à la fois.
- **Les durées** ne se comparent pas : plusieurs passages ont tourné en même
  temps.

## Suite

1. **Le détecteur sur les rushs de l'auteur** : ses seuils viennent de C-PROM
   (micros de radio et de terrain, autres voix) ; les marqueurs « M » du kit
   des jointures diront s'il tient sur des interviews filmées, et avec
   NeMo, l'aligneur du moteur, comme avec MFA. Avant tout branchement dans
   le moteur, comme une alerte de plus à côté du son hors des mots.
2. **Les marqueurs « M » de l'auteur** sur ses rushs (kit des jointures) : une
   vérité sur ses propres voix, et, plus tard, de quoi entraîner un
   détecteur sur des données qui lui appartiennent. C-PROM, non commercial,
   ne peut pas servir à entraîner.
3. **Qwen3-ASR pour le texte** reste une piste séparée des hésitations :
   22 % d'erreurs de moins hors hésitations, à confirmer sur des rushs
   avant d'envisager de remplacer Whisper, qui écrit au moins quelques
   « euh ».
