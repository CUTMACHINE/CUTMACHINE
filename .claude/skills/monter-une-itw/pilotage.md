# Piloter le moteur sans aller-retour

Le contrat du moteur, et les pièges que ses réponses ne disent pas.

## Passer par le serveur

Monte par les outils MCP du moteur, jamais par `--call-tool` dans un shell ;
si le serveur n'apparaît pas dans les outils, dis-le. Ne lis jamais les
fichiers du projet ni ses caches à la main : ce qu'un outil ne rend pas se
signale dans `frictions`, il ne s'extrait pas.

## Un son d'enregistreur à part

Un son d'enregistreur (un fichier `Mix`, un micro-cravate) se cale dans le
moteur, avant le dérushage, plutôt que dans Resolve : `measure_audio_offset`,
puis `sync_audio`. Tout suit ensuite, `create_interview_short` compris.

## Monter dans l'app ouverte

Quand `open_project` répond `app: true`, tu montes sous les yeux de la
personne, dans le même historique que ses gestes.

- Encadre chaque consigne par `begin_intervention` et `end_intervention` :
  elle s'annule alors d'un seul `Cmd+Z`.
- Un marqueur nommé « Marqueur » est un endroit désigné sans note (posé
  d'une touche, `M`) : demande ce qu'il y a vu, ou va lire ce qui s'y passe
  (`inspect_cut`, `list_timeline_words`).
- `create_project` et `ingest_media` ne passent pas par l'app : importe les
  rushes avant, ou demande-lui de le faire. Si l'app se ferme en cours de
  route, l'appel en vol répond `AppClosed` : rappelle `open_project`, qui
  ouvre alors le projet dans le serveur.

## Trois révisions

Chacune pour sa question — ne les échange pas :

| Révision | Où tu la lis | Ce qu'elle protège | Refus |
|---|---|---|---|
| `revision` du projet | toute réponse du serveur | une mutation (`expected_revision`) | `StaleRevision` |
| `doc_hash` de la timeline | tout résultat d'édition, `prepare_edit_plan` | un plan d'agent (`base_doc_hash`) | `StaleRevision` |
| `transcript_revision` | `get_timeline_transcript`, `list_speech_blocks` | des `span_id` / `block_id` | `StaleSelection` |

Une édition ailleurs dans le projet périme la première, pas la deuxième ; un
étalonnage périme les deux premières, pas la troisième.

## Après la création du montage

`create_interview_short` nomme la nouvelle timeline dans sa réponse
(`created_timeline_ids`) : passe cet ID en `timeline_id` à chaque appel qui
suit. Si la création échoue, arrête ce qui en dépend : l'active est encore le
dérushage, et c'est lui qu'un resserrement refermerait.

Deux spans séparés de plus qu'un souffle, que `create_interview_short`
refuse de coller, sont souvent deux départs : prends le second et rallonge
sa tête par `ripple_trim` (« Bon, ce qu'on voulait », puis « Ce qu'on
voulait faire, en fait… »).

## Désigner le bon plan quand un rush en fournit plusieurs

Un même rush alimente souvent plusieurs blocs du montage : « le premier » ou
« le dernier » vise à côté. Trie les plans du rush par `source_in` et nomme le
rang. Applique les corrections de bordure **avant** le resserrement : après, le
rush est éclaté en morceaux et le rang ne désigne plus rien.

## Un refus arrête les opérations qui en dépendent

Un refus (`ok: false`, `isError`) ou une réponse absente arrête ce qui en
dépend ; `applied: false` n'est pas une modification. Relis les bornes
avant de reformuler, ne répète pas la même requête.
