# Monter en phases : ce que chaque agent relit

À chaque appel, le modèle relit tout son contexte, chaque réponse d'outil
comprise. Ce qu'une phase a lu, la suivante n'en a besoin qu'en résumé.

Un montage complet se fait donc en **quatre phases**, chacune repartant d'un
contexte neuf avec un **fichier d'état** qui porte ce que la suivante doit
savoir. Signale dans `frictions` toute phase où le fichier d'état n'a pas
suffi.

## Les quatre phases

| Phase | Étapes de [SKILL.md](SKILL.md) | Ce qu'elle lit | Ce qu'elle laisse dans l'état |
|---|---|---|---|
| 1. Dérushage | 1 à 4 | transcriptions, enveloppes, alignements MFA, analyses | chaque rush : parole ou muet, aligné ou non, ce que sa réponse signale |
| 2. Propos et sélection | 5 et 6 | toute la parole de la personne | `propos`, `blocs_retenus` avec leur texte, les idées laissées de côté |
| 3. Assemblage et resserrement | 7 à 12 | bornes, plans, `cut_words`, `list_timeline_words`, `transcribe_timeline` | la timeline du montage, sa durée, les compteurs de `list_timeline_words`, les écarts de texte restants, les plans écartés |
| 4. Habillage et livraison | 13 à 15, puis les contrôles | plans de coupe, cartons, couleur, export | les fichiers rendus, les contrôles faits, les exceptions écrites |

La phase 2 ne se délègue pas : elle est la décision du film, et celui qui la
prend doit avoir lu la parole. Le test de l'antécédent et la relecture finale
du texte monté restent aussi à l'agent qui a reçu la consigne, comme le brief
précisé (étape 0, [brief.md](brief.md)), qui se fait avant la phase 1 et
s'écrit dans l'état, sous `compte_rendu.brief`.

## Le fichier d'état

**Où.** À côté du paquet, jamais dedans :
`<dossier du paquet>/<nom du paquet>.etat-<timeline_source>.json`, où
`<nom du paquet>` est le nom du dossier sans `.cutmachine-project` et
`<timeline_source>` l'ID de la timeline de dérushage. Pas dans `.cutmachine/`,
qui se régénère et s'efface ; pas dans le paquet, qui se collecte et se
partage. Jamais dans un dépôt git : il contient la parole des personnes
filmées.

**Quand.** À la fin de chaque phase, en entier (le fichier est réécrit,
jamais complété à la main), et avant de répondre à qui a lancé la phase.

**Forme.** `compte_rendu` est exactement le bloc de fin de [livraison.md](livraison.md) :
le compte rendu final le recopie, il n'est pas rédigé une seconde fois. Les
temps s'écrivent comme les outils les rendent.

```json
{
  "etat_montage": 1,
  "projet": "/chemin/absolu/Film.cutmachine-project",
  "consigne": "la consigne, telle qu'elle a été donnée",
  "phase_terminee": 2,
  "timeline_montage": null,
  "rushes": [
    {"media_id": "…", "nom": "…", "parole": true, "aligne": true,
     "remarque": null}
  ],
  "montage": {
    "duree": null,
    "hesitations_remaining": null,
    "restarts_remaining": null,
    "ecarts_de_texte": []
  },
  "livraison": {"fichiers": [], "controles": [], "exceptions": []},
  "a_faire": [],
  "compte_rendu": {
    "compte_rendu": 1,
    "brief": {"compris": [], "questions": []},
    "propos": "…",
    "timeline_source": "…",
    "blocs_retenus": [],
    "plans_ecartes": [],
    "frictions": []
  }
}
```

- `rushes` : une ligne par rush ingéré. `remarque` dit en une phrase ce que
  sa réponse a signalé (`likely_hallucinated`, `uncovered_speech` comblé ou
  non, `likely_incomplete`) ; `null` sinon.
- `montage` : rempli par la phase 3, depuis le `summary` de
  `list_timeline_words` et la dernière comparaison de texte. `ecarts_de_texte`
  liste en une phrase chaque écart laissé, avec sa raison.
- `a_faire` : ce que la phase suivante doit savoir et qui n'a pas de champ,
  une phrase chacun. « Le plan du rush 12 est à l'épaule, gardé exprès » ;
  pas « voir plus haut ».
- Aucune réponse d'outil n'y est recopiée : un identifiant, un compteur, une
  phrase. Ce qu'un outil rend, la phase suivante le redemande si elle en a
  besoin.

## Avec des sous-agents

Quand l'agent peut lancer des sous-agents (Claude Code), celui qui a reçu la
consigne coordonne :

1. Il précise le brief (étape 0) et écrit le fichier d'état avec `consigne`,
   `projet` et `compte_rendu.brief`, `phase_terminee` à 0, puis lance la
   phase 1 dans un sous-agent. La phase 1 ne dépend pas des réponses : elle
   peut tourner pendant qu'on les attend.
2. Il fait lui-même la phase 2, depuis l'état et les lectures qu'elle
   demande, et écrit l'état.
3. Il lance la phase 3, puis la phase 4, chacune dans un sous-agent, l'une
   après l'autre : elles éditent la même timeline.

La consigne d'un sous-agent tient en cinq points : le chemin du fichier
d'état ; le numéro de la phase ; les fichiers du skill à lire, `SKILL.md`
compris (par le dépôt, ou par `get_procedure` hors du dépôt) ; « donne
`timeline_id` à chaque appel » ; « écris l'état, puis réponds en moins de
300 mots : ce qui est fait, ce qui a coûté, ce qui reste ». Le sous-agent
utilise le même serveur et le projet déjà ouvert : il ne le rouvre pas.

Entre deux phases, le coordinateur lit le fichier d'état, pas le détail de
ce qu'a fait le sous-agent. Après la phase 3, il lit le texte du montage
(`get_timeline_transcript`) et refait le test de l'antécédent ; un défaut
renvoie la phase 3 avec la demande précise écrite dans `a_faire`, sans la
refaire en entier.

## Sans sous-agents

- **Avec des fichiers** (Codex, un client qui écrit sur le disque) : le même
  fichier d'état. Si le client sait compacter sa conversation, il le fait
  juste après avoir écrit l'état, jamais au milieu d'une phase.
- **Sans fichiers** (le panneau Agent de l'app, qui n'a que les outils du
  moteur) : la réponse de fin de phase se termine par le bloc d'état en
  JSON, pour qu'une nouvelle conversation puisse repartir de lui.

## Ce qui ne se coupe pas

- Une phase ne s'arrête pas entre `prepare_edit_plan` et `apply_edit_plan`
  ni au milieu d'une intervention : termine-la (`end_intervention`) ou
  abandonne le plan (`discard_edit_plan`) et écris pourquoi dans
  `plans_ecartes`.
- Les règles de [SKILL.md](SKILL.md) valent dans chaque phase : un
  sous-agent qui ne l'a pas lu les enfreindra.
