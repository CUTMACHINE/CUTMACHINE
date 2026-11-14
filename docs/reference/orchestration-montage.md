# Étapes durables et mesures d’agent

DB3, OR1–OR3 — `DONNEES-2026-10`, 6 octobre 2026.

Le moteur sait enregistrer une étape de montage avec son plan exact, ses
journaux et une capture SQL, puis la retrouver après une coupure sans
rééditer. Tout client MCP peut s’en servir : un agent qui pilote le moteur
directement, le panneau Agent de l’app, ou un futur pilote. Le panneau écrit
aussi une trace ACP que l’on peut mesurer après coup.

Le pilote LangGraph externe qui utilisait ces reçus a été retiré le
6 octobre 2026 (décision et raisons dans
`DONNEES-2026-10`).
Son dernier état, graphe v3 compris, reste dans l’historique Git au commit
`1f60686`.

## Ce qui empêche une double édition

Le client prépare d’abord son plan exact (`prepare_edit_plan`, puis
`get_edit_plan`) et conserve sa demande. Le moteur committe ensuite, dans sa
transaction multi-fichiers habituelle, le projet, les journaux, un reçu
`.cutmachine-run-step-<ULID>.json` et la capture en attente avec son
contexte. Le reçu conserve la demande, les révisions avant/après, le plan
exact, le journal de la timeline et le journal projet.

`commit_run_step` prend `request_json`, une chaîne contenant `run_id`,
`step_id`, `stage`, `timeline_id`, `base_revision`, `base_history_hash`,
`base_project_history_hash`, `plan_json` et `observations_json`. Les IDs sont
des ULID complets ; les trois empreintes sont celles du moteur.
`get_run_step` prend `step_id`. Un reçu absent rend aussi `history_hashes`
par timeline et `project_history_hash` pour préparer la demande. Un reçu
présent rend `receipt_json`, `revision` et `matches_current`. Un reçu dont
la timeline a été supprimée se lit encore, `matches_current` à faux.

Si le commit a réussi mais que la réponse s’est perdue, la même demande
retrouve le reçu. Elle ne réédite rien. Une autre demande sous le même ID est
refusée `RunStepIdReused`. Une révision ou un journal différent refuse
`RunStateChanged` ou `StaleRevision`, même si une annulation a ramené le
document aux mêmes octets. Les empreintes distinguent les états ; elles ne
sont pas une authentification cryptographique. Le rejeu du reçu compare aussi
les octets exacts des deux journaux.

Le CLI expose `--commit-run-step <projet> '<demande-json>'` et
`--get-run-step <projet> <ULID>`. Le backend en mémoire refuse le commit : une
reprise nécessite le paquet sauvegardé, pas un cache de réponses de session.
Dans l’app, le panneau Agent lit les révisions avant/après du reçu lui-même :
une édition humaine intervenue avant la réponse ne lui fait pas attribuer à
l’agent un changement que son étape n’a pas produit, et une étape sans plan
n’ajoute pas de ligne d’édition.

## Légendes préparées dans un plan

`generate_captions` se prépare comme les autres éditions : le backend
projette les mots des caches sur la copie de travail, après les étapes qui
la précèdent dans le même plan. Une lecture du son monté n’est utilisée que
si son identité correspond à ce document. Préparer les légendes ne change ni
le projet ni les journaux ; appliquer le plan ajoute une seule entrée
annulable. `generate_captions` avec `preview:true` rend les cartons sans
rien éditer.

## Captures et requêtes SQL

L’archive native est passée au schéma 2 pour les reçus, puis au schéma 3
le 24 octobre 2026 (DEC44, [Archive SQL](archive-projets.md#la-migration-vers-le-schéma-3)) :
les captures y sont rangées par empreinte, `project_snapshots` est une vue
qui rend chaque document à l'octet, et `project_run_steps` garde ses lignes,
liées à `archive_snapshots`. Un écrivain migre une base v1 ou v2 ; ses
anciennes captures se relisent à l'octet près. Un lecteur peut consulter v1
ou v2 sans la modifier. Une ancienne version du moteur refuse ensuite v3.

`project_run_steps` relie `step_id`, `run_id`, `stage`, `project_id`,
`timeline_id`, `snapshot_id`, `base_revision`, `result_revision`, `receipt_json`.
La capture et cette ligne partagent le commit SQL. Les triggers refusent les
remplacements, modifications et suppressions. Dans le paquet,
`.cutmachine-archive-<ULID>.context.json` accompagne le JSON en attente.
Un refus SQL laisse montage et reçu acquis ; ouverture/synchronisation
réessaient de livrer les deux. `CUTMACHINE_PROJECT_ARCHIVE=off` conserve les
reçus dans le paquet mais ne crée pas de capture SQL.

```sql
SELECT r.run_id, r.stage, s.created_at, r.result_revision,
       json_extract(r.receipt_json, '$.request_json') AS request_json,
       s.document_json
FROM project_run_steps r
JOIN project_snapshots s USING(snapshot_id)
ORDER BY r.run_id, s.created_at, s.snapshot_id;
```

Les intentions et le contexte sont du JSON dans `request_json` ; les
observations sont à leur tour une chaîne JSON. La base ne contient pas de
copie des médias. La croissance de l'archive est traitée par le schéma 3
(DEC44) ; restent dans les pistes l’indexation complète des analyses et les
liens avec les éditions effectuées hors des étapes durables.

## Mesurer une session du panneau Agent

ACP (Agent Client Protocol) est le protocole par lequel le panneau Agent
confie les consignes à Claude Code, Codex ou un autre agent, qui appelle
ensuite le moteur par MCP. Le panneau écrit ses traces sous
`~/Library/Logs/CUTMACHINE/agent/`.

```sh
python3 tools/acp-report/acp_report.py --trace /chemin/horodatage.acp.jsonl
```

L’outil demande seulement Python 3.9 ou supérieur, sans moteur ni dépendance.
Il lit la trace sans la modifier et n’invente aucune mesure qu’un agent n’a
pas fournie.

Le bilan compte des **tours**, pas les appels modèle internes à l’agent.
Sa couverture reste celle du fichier (`trace_only`). Une réponse sans
requête conservée garde ses compteurs, avec une durée inconnue ; un même
identifiant d’outil réutilisé dans un nouveau tour compte un nouvel appel.
Les notifications d’occupation du contexte ne sont pas de l’usage facturé.
Les compteurs absents, négatifs ou invalides ne valent pas zéro token :
`known_totals` conserve les compteurs connus, `reported_by_field` dit
combien de réponses portent chaque champ, `lower_bound_fields` nomme ceux qui
manquent dans une partie des réponses, et `totals_are_lower_bounds` et
`duration_is_lower_bound` signalent les bilans incomplets. Les noms des
compteurs restent ceux du fournisseur ou d’ACP, y compris les catégories de
cache ; les sous-champs numériques apparaissent avec leur chemin séparé par
un point. Certains champs incluent d’autres compteurs : ne pas les
additionner entre eux. Les durées sont inclusives et peuvent se chevaucher.
`api_cost` reste inconnu et `acceptance` non relevé.

## Vérification

`cutmachine_run_step_tests` est portable ; `cutmachine_run_step_storage_tests`
exerce les reçus, l’archive et le protocole de transaction du paquet.
`cutmachine_mcp_tools_tests` couvre les outils MCP des reçus ;
`cutmachine_mcp_endpoint_tests` les événements qu’un reçu transmet au panneau
Agent (étape d’observation, étape éditée, rejeu) ; `cutmachine_mcp_tests` la
préparation des légendes dans un plan. `cutmachine_acp_report_tests`,
portable, lit des traces ACP réelles et incomplètes et passe par la ligne de
commande. Depuis le retrait du pilote, aucun test ne rejoue une étape
durable à travers la fenêtre de l’app.

```sh
ctest --test-dir build --output-on-failure -R 'run_step|acp_report|mcp_'
```
