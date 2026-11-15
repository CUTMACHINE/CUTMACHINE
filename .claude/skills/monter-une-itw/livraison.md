# Livrer : contrôles et compte rendu

La liste des contrôles est dans [SKILL.md](SKILL.md), « Avant de rendre ».
Ce fichier dit ce qu'ils laissent à écrire, et la forme du bloc qui termine
le compte rendu ; [phases.md](phases.md) en recopie la forme dans son
fichier d'état.

## Ce que les contrôles laissent à écrire

- **`ffprobe`** : vérifie d'abord que le fichier existe. Sur un chemin
  absent, il sort une chaîne vide, et une comparaison avec du vide passe
  pour un résultat.
- **`propose_grade`** : toute exception à la tolérance (une scène voulue
  sombre, un contre-jour gardé, un plan `limited_by_highlights` regardé et
  laissé) s'écrit dans le compte rendu.
- **`review_cuts`** : ses alertes sont à examiner ; un jump cut peut être
  voulu. Reprends ce qui est involontaire et liste ce que tu gardes, avec
  sa raison.
- **`check_timeline`** : sa politique par défaut interdit le fondu au noir
  final ; `final_black: "allowed"` seulement si le brief en veut un. Quand
  la marque a sa charte dans la bibliothèque, crée le style des cartons par
  `add_caption_style` avec `kit`, et passe `kit` dans `policy`.

## Le bloc qui termine le compte rendu

Le compte rendu final se termine par un bloc JSON à forme fixe. Il ne
remplace pas le texte : il garde ce que seul le chat garde, et qui se perd
avec la session. Le projet dit ce qui a changé, jamais pourquoi. Le panneau
Agent le rend consultable derrière « Afficher le rapport technique ».

```json
{
  "compte_rendu": 1,
  "brief": {
    "compris": [{"point": "…", "texte": "…", "source": "…"}],
    "questions": [{"question": "…", "hypothese": "…", "reponse": null}]
  },
  "propos": "la phrase de l'étape 5",
  "timeline_source": "ID de la timeline où les blocs ont été lus",
  "blocs_retenus": [
    {"ordre": 1, "source_id": "…",
     "source_in": {"value": 0, "rate": 25},
     "duration": {"value": 0, "rate": 25},
     "texte": "…", "block_id": "B3", "transcript_revision": "…"}
  ],
  "plans_ecartes": [{"plan_id": "…", "raison": "…"}],
  "frictions": [{"outil": "…", "ce_qui_a_coute": "…"}]
}
```

- `brief` : sa forme est dans [brief.md](brief.md) ; `null` pour une
  retouche, qui n'a pas de brief à préciser.
- `propos` : la phrase de l'étape 5, pas un résumé du film rendu.
- `blocs_retenus` : dans l'ordre du film, avec leur plage exacte dans le
  rush et leur texte ; `block_id` et `transcript_revision` quand la
  sélection est passée par `list_speech_blocks`, `null` sinon.
- `plans_ecartes` : chaque plan d'édition préparé puis écarté
  (`discard_edit_plan`) ou laissé sans suite, avec sa raison en une phrase.
- `frictions` : un refus, un outil qui manquait, une étape faite hors du
  moteur, une réponse trop lourde pour être lue ; une phrase chacune.
- Rien ne s'omet : une liste vide s'écrit `[]`, une valeur inconnue `null`.
