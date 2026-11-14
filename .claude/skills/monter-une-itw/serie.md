# Monter une série : plusieurs films d'un même projet en même temps

Une série : plusieurs interviews d'un projet (une timeline de dérushage par
personne) montées en même temps par plusieurs agents, chacun sur sa
timeline, par un même serveur. Le brief se précise une fois pour toute la
série ([brief.md](brief.md)).

## Avant de lancer les agents

- **Un serveur, le projet en mode strict.** Ouvre le projet une fois, pour
  tout le serveur : `open_project` avec `require_timeline: true` (ou
  `--mcp-serve <projet> --stdio --require-timeline`). La réponse dit
  `require_timeline`.
- **Pas de série dans l'application ouverte.** Quand l'app tient le projet,
  le serveur lui relaie chaque appel : une édition d'une autre timeline y
  bascule l'affichage, et la lecture d'une timeline non affichée y est
  refusée. Ferme le projet dans l'app, puis sers-le par `--mcp-serve`.
- **Une timeline par agent**, nommée dans sa consigne par son identifiant.
  Chaque agent crée son film dans une nouvelle timeline
  (`create_interview_short`, qui n'active rien en mode strict).

## Pendant la série, chaque agent

- **Préfixe tes clés d'idempotence** de ton nom (« itw2-insert-1 »). Une clé
  déjà prise par un autre appel est refusée `IdempotencyKeyReused`, jamais
  servie avec la réponse de l'autre.
- **Ne change pas de projet** pendant une série ; un `open_project` raté
  laisse le projet ouvert pour tous, et le refus le dit.
- **Ne touche pas à ce qui est partagé** : gestion couleur, chutiers, rushes
  et compositions appartiennent au projet. En série, on n'y touche qu'avant
  de lancer les agents, ou par un seul d'entre eux.

Le 3 octobre 2026, quatre interviews ont été montées ainsi : aucune timeline
d'un autre touchée, seules des attentes dans la file des tâches.

## Ce que rend une série

Chaque agent rend son film (sa timeline, son export vérifié par
`verify_export`) et son compte rendu, avec le bloc JSON de fin que demande
[livraison.md](livraison.md). Chacun nomme ses interventions (`end_intervention`),
même d'un seul pas : c'est ce qu'on lira dans l'historique de sa timeline
(`describe` avec `only: ["history"]` et son `timeline_id`), qui commence à
sa première édition (la création est au journal du projet). Celui qui
coordonne la série relit les comptes rendus et ces historiques, liste les
frictions (refus, attentes, révisions qui ont bougé) et donne à l'éditeur la
liste des films avec, pour chacun, ce qu'il dit en deux phrases.
