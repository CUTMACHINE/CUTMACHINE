---
name: monter-une-itw
description: Monter une interview filmée (portrait, témoignage, micro-trottoir) dans un projet CUTMACHINE et la livrer — cohérence du propos, frontières de mots MFA, resserrement, plans de coupe, étalonnage, export, sous-titres, renvoi vers DaVinci Resolve. À utiliser quand on demande de « monter » une interview, d'en refaire l'export, d'y ajouter des plans d'illustration, d'en sortir les sous-titres ou de la renvoyer dans Resolve.
---

# Monter une interview avec CUTMACHINE

## La règle qui prime sur toutes les autres

**Un montage d'interview est d'abord un propos qui se tient.** Un film
peut avoir tous ses chiffres bons et ne pas se tenir : plans par minute,
part de B-roll, part de silence et CPS repèrent des anomalies. Ils se
lisent après, ils ne se visent pas, et la durée résulte du propos.

### Le test de l'antécédent

Relis le texte retenu comme quelqu'un qui n'a pas vu les rushes : un mot
qui renvoie (`ça`, `c'est`, `également`, `ce projet`) sans antécédent dans
le montage est une faute, pas une imperfection : une ouverture au milieu
d'une phrase, un « c'est là » dit avant le nom du lieu, un « pareil » gardé
sans la phrase qu'il reprenait.

- Garde l'antécédent avec sa conséquence, ou écarte les deux.
- Ne réordonne un bloc que si ses références survivent au déplacement.
- Préfère peu de blocs longs et complets à beaucoup de morceaux courts.
- Pas de digression technique dans une minute : quinze secondes sur le
  choix d'un matériau passent avant le propos.

## Avec la personne qui signe

Le film appartient à la personne qui le signe : avant de monter comme
avant de reprendre un montage, lis ce qui en découle dans
[brief.md](brief.md#avec-la-personne-qui-signe).

## Rien de généré sur la personne filmée

Ni voix, ni visage, ni corps : rien qui lui fasse dire ou faire ce qu'elle
n'a ni dit ni fait. Le 9:16 d'une interview passe par le cadrage
(`set_clip_framing`, `track_subject_framing`), jamais par l'extension
d'image.

Une seule exception : le doublage et la synchronisation des lèvres, quand
la personne qui signe dit que la personne filmée a donné son accord. Tu ne
les proposes pas de toi-même, et tu n'affirmes jamais cet accord à sa place
(`consent_of_filmed_person`).

## Où trouver le détail

Chaque fichier se lit quand l'étape l'appelle.

| Fichier | À lire |
|---|---|
| [brief.md](brief.md) | avant de monter ou de reprendre un montage : ce que tu as compris, les films précédents de la série, tes deux ou trois questions |
| [mots-et-bornes.md](mots-et-bornes.md) | avant d'aligner les mots ou de poser une borne ; quand un bloc s'ouvre sur la question de l'intervieweur |
| [resserrement-et-finition.md](resserrement-et-finition.md) | pour resserrer, fermer les creux, chasser les « heu » et relire le texte monté |
| [pilotage.md](pilotage.md) | pour partir d'un dossier de rushes, avec ou sans son d'enregistreur ; dans l'app ouverte ; quand un outil refuse ; avant d'adresser un plan dans un rush découpé |
| [plans-de-coupe.md](plans-de-coupe.md) | pour choisir et poser les plans de coupe, et traiter les jump cuts |
| [sous-titres.md](sous-titres.md) | pour produire ou reprendre des cartons, dans le moteur ou dans Resolve |
| [couleur-et-export.md](couleur-et-export.md) | pour étalonner et exporter |
| [livraison.md](livraison.md) | avant de rendre : ce que les contrôles laissent à écrire, le bloc JSON du compte rendu |
| [resolve.md](resolve.md) | pour renvoyer dans DaVinci Resolve, y compléter des sous-titres ou illustrer avec du matériel extérieur |
| [serie.md](serie.md) | quand plusieurs interviews d'un même projet se montent en même temps, par plusieurs agents |
| [phases.md](phases.md) | avant un montage complet : les quatre phases, le fichier d'état, ce qui se confie à un sous-agent |

## Dire pourquoi on coupe : `motif`

Donne `motif` à chaque geste qui ne vient pas d'une règle : les coupes
(`remove_words`, `remove_source_ranges`, `delete_timeline_range`,
`ripple_trim`, `roll_edit`), les plans de coupe, `clone_timeline`,
`create_interview_short`, `end_intervention` et `apply_edit_plan` (un pas de
plan prend le sien dans ses `arguments`). Le vocabulaire est dans
`list_motifs` ; les raisons qui se confondent : `duplicate` (la même idée
dite deux fois, tu gardes la meilleure prise), `selection` (ce que le film
garde du propos, et dans quel ordre), `marker` (un marqueur de la personne
le demande, avec son `marker_id`), `technical` (synchro, image-éclair,
raccord cassé). La `note` dit en une ligne ce que la raison ne dit pas.

Quand la personne choisit entre deux versions, écris-le
(`choose_timeline_variant`, `author: "person"`, sa phrase en `note`) ; relis
ses choix (`list_motifs`, `journal: "project"`) avant de proposer des
variantes du même genre.

## L'ordre de travail

Chaque étape produit ce dont la suivante a besoin. Un montage complet se
fait en quatre phases, chacune dans un contexte neuf avec un fichier d'état
→ [phases.md](phases.md).

0. **Préciser le brief** → [brief.md](brief.md). Rien ne monte avant ; les
   analyses tournent pendant.
1. **Analyser l'enveloppe** : `analyze_speech_onset` et
   `analyze_shot_quality` sur tous les rushes en un appel (`media_ids`), en
   tâche, avant la transcription, qui s'en sert.
2. **Transcrire tous les rushes en verbatim**, en un appel et en tâche,
   avec `language: "fr"` pour une interview en français : le décodage des
   « euh » suit alors les réglages du moteur, sans le nommer ; compte large :
   au moins la durée des rushes.
3. **Classer parole ou plan muet** d'après la réponse (`text`,
   `likely_hallucinated`) : whisper hallucine sur le silence, et un plan de
   coupe peut porter du son.
4. **Aligner les rushes parlés** (`align_words`) et combler les trous de
   texte qu'il signale, puis `analyze_hesitations` sur les mêmes rushes.
   → [mots-et-bornes.md](mots-et-bornes.md)
   Si un rush porte plusieurs voix (l'intervieweur qu'on entend, deux
   personnes), `analyze_speakers` avec le nombre de voix que tu as compté
   en lisant, puis `list_speakers` : chaque mot de `list_source_words`
   porte ensuite son `speaker`. Une voix est un numéro, que tu nommes
   d'après ses mots ; au-dessus de 850 de `similarity`, ne te fie pas au
   partage et dis-le dans le compte rendu.
5. **Lire toute la parole de la personne** comme un script, et écrire son
   raisonnement en une phrase : d'où elle part, ce qu'elle a fait, pourquoi
   ça compte, où ça la mène.
6. **Choisir des blocs entiers** (`list_speech_blocks`,
   `inspect_speech_selection`) et garder leur texte, leurs plages et la
   timeline source. L'ouverture se comprend sans rien savoir avant : jamais
   un bloc `opens_mid_sentence`, situer n'est pas présenter, et un intitulé
   posé seul ne dit rien. La présentation de la personne (qui elle est,
   d'où elle vient) dépend de l'interview : juges-en, mais écarte-la chaque
   fois qu'on peut s'en passer.
7. **Poser les bornes** sur `entry_frame`/`exit_frame` de
   `list_source_words` ; une plage = un plan.
   → [mots-et-bornes.md](mots-et-bornes.md)
8. **Monter les blocs** par `create_interview_short` (le dérushage en
   `timeline_id`, `block_id` et `transcript_revision`), et corriger les
   bordures (`trim_boundary_air`) avant le resserrement.
   → [pilotage.md](pilotage.md)
9. **Resserrer** : `tighten_pauses` sur tous les plans de parole en un appel
   (`clip_ids`), puis `close_junction_air` sur tous les raccords (`pairs`).
   Un mot emporté sans être visé (`carried` de `cut_words`) est perdu :
   répare-le.
   → [resserrement-et-finition.md](resserrement-et-finition.md)
10. **Purger les questions de l'intervieweur**, repérées au creux et au
    niveau. → [mots-et-bornes.md](mots-et-bornes.md)
11. **Finition** : `list_timeline_words` jusqu'à un `summary` à zéro, ou
    chaque reste justifié dans le compte rendu. Un redémarrage (`restarts`)
    que l'outil signale se vérifie sur les mots avant d'être coupé : le
    détecteur marque aussi des mots longs entiers.
    → [resserrement-et-finition.md](resserrement-et-finition.md)
12. **Comparer le texte choisi, le texte du projet et le texte du son
    monté** (`transcribe_timeline`, verbatim et sans, dans la langue des
    rushes), et résoudre les écarts qui touchent le sens. Deux fois, pas
    plus : montage posé, puis avant de rendre ; entre deux retouches,
    `list_timeline_words` et `cut_words` suffisent.
13. **Poser les plans de coupe** sur le sens, puis ne laisser aucune coupe
    dans un même rush à nu sans raison dite.
    → [plans-de-coupe.md](plans-de-coupe.md)
14. **Nom à l'écran ou carton**, s'ils sont demandés : `add_motion_template`,
    puis `place_motion_clip` sur le span où la personne commence à parler ;
    un refus de mise en page se corrige par les props, pas par
    `allow_layout_issues`.
15. **Couleur, puis livraison.** Un étalonnage de base toujours, même non
    demandé : `conform_color_management`, puis `propose_grade` sur les
    visages. Livre ce qui est demandé, un master seulement si on le demande :
    « remonte la timeline » veut une timeline Resolve, pas un fichier.
    → [couleur-et-export.md](couleur-et-export.md),
    [sous-titres.md](sous-titres.md), [resolve.md](resolve.md)

## Avant de rendre

- `check_timeline` avant l'export, puis avec `path` sur le fichier final ;
  pour une interview, `policy` avec `jump_cuts: "forbidden"` et les durées
  du brief ;
- `review_cuts` après la dernière coupe, puis `inspect_cut` sur ce qu'il
  signale ;
- `propose_grade` une dernière fois, et `analyze_timeline_audio` ;
- `check_caption_legibility` si des légendes sont incrustées ;
- `export_timeline` avec `verify: true`, puis `ffprobe` pour la résolution,
  la cadence et le son ;
- pour un renvoi dans Resolve, les contrôles de [resolve.md](resolve.md).

Un contrôle `not_run` n'est pas un vert. Ce que chacun laisse à écrire est
dans [livraison.md](livraison.md). Aucun ne voit l'ennui ni l'incohérence
d'un propos : seule la relecture du texte monté, comme quelqu'un qui n'a
pas vu les rushes, juge le récit.

## Le compte rendu

Le bilan pour la personne tient en une centaine de mots : le livrable avec
un lien cliquable, sa durée, puis les réserves et les choix à relire. Dis
quels contrôles tu as faits et ce que tu n'as pas pu juger (planches, texte
retranscrit, écoute, film rendu) ; ne dis pas que toutes les règles sont
respectées si tu en laisses une insatisfaite. Il se termine par le bloc
JSON de [livraison.md](livraison.md).
