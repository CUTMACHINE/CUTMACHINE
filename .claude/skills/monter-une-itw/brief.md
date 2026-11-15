# Préciser le brief avant de monter

Une version se jette entière quand le brief a été mal compris, presque
jamais autrement : une minute de validation coûte moins que quarante de
montage sur un malentendu. Les commandes arrivent souvent minces : un
ticket de deux lignes, un message.

Ce fichier sert les trois procédures : interview, reel ou pub, motion.

## Avec la personne qui signe

Le film appartient à la personne qui le signe ; l'agent monte pour elle, et
ce qu'elle lui a confié, il le décide seul. Cinq gestes en découlent :

- **Avant de monter, dis ce que tu as compris du brief**, tes hypothèses et
  deux ou trois questions : la suite de ce fichier.
- **Avant de reprendre un montage, lis ses marqueurs** (`describe`,
  `only: ["markers"]`) et dis dans le compte rendu ce que tu as fait de
  chacun ; pour demander un regard, pose un marqueur `question`.
- **Quand le résultat n'est pas sûr, avance sur une copie**
  (`clone_timeline`) : `undo` défait ton dernier geste, il ne sert pas à
  revenir à une version.
- **N'affirme pas « mieux », montre** : ce que le moteur prouve se dit ; le
  reste se montre par une variante, et la personne tranche. Un désaccord se
  dit tel quel, sans l'amortir.
- **Dis quelle couche tu sers** : le quotidien (son look, sa sonie, sa
  charte) ou le plafond, qui va plus loin et demande son attention.

## Quand

- **Avant la première opération qui monte** : avant `create_interview_short`,
  le premier `insert_clip` ou la première composition. Les lectures, elles,
  peuvent partir avant et pendant : ce que disent les rushes fait partie du
  brief.
- **Quand une version a été rejetée en entier** : c'est le brief qui a été
  mal compris. Refais cette étape, et dis ce que tu comprends autrement
  que la fois d'avant.
- **Pas pour une retouche** : refaire un export, déplacer un carton,
  décliner dans un autre format un film validé. Pas non plus quand l'état
  d'un montage en phases porte déjà le brief précisé : relis-le
  ([phases.md](phases.md)).

## Trois sources, lues dans cet ordre

1. **Le brief tel qu'il arrive** (ticket, message, mail transféré). Garde-le
   mot pour mot : c'est la `consigne` du fichier d'état.
2. **Les films précédents de la série.** Ce qui a déjà été livré à ce
   client, pour cette série, répond déjà au format, à la durée et au style
   des légendes. L'archive des projets les garde (`query_project_archive`, sans
   projet ouvert, `database` omis) : cherche-les par le début de nom que
   les projets de la série partagent (ici `Serie`), puis par celui des
   autres projets du même commanditaire. Pour chaque timeline de la
   dernière capture de chaque projet, cette requête rend le format, la
   durée, la police des légendes et le nombre de marqueurs :

   ```sql
   WITH dernier AS (
     SELECT project_id, MAX(created_at) AS at FROM project_snapshots
     WHERE json_extract(document_json, '$.name') LIKE 'Serie%'
     GROUP BY project_id)
   SELECT json_extract(s.document_json, '$.name') AS projet,
          json_extract(t.value, '$.name') AS timeline,
          json_extract(t.value, '$.width') || 'x' ||
            json_extract(t.value, '$.height') AS format,
          ROUND(MAX(json_extract(c.value, '$.timeline_in.value') * 1.0 /
                      json_extract(c.value, '$.timeline_in.rate') +
                    json_extract(c.value, '$.duration.value') * 1.0 /
                      json_extract(c.value, '$.duration.rate')), 1) AS duree_s,
          json_extract(t.value, '$.caption_styles[0].font_family') AS police,
          json_array_length(t.value, '$.markers') AS marqueurs
   FROM project_snapshots s
   JOIN dernier d ON d.project_id = s.project_id AND d.at = s.created_at,
        json_each(s.document_json, '$.timelines') t,
        json_each(t.value, '$.tracks') k,
        json_each(k.value, '$.clips') c
   GROUP BY s.snapshot_id, t.key
   ORDER BY projet, timeline
   ```

   Ne compte que les films, pas les timelines de dérushage ou de travail,
   qui se reconnaissent à leur nom. Hors de l'archive, `ffprobe` sur les
   fichiers livrés donne la durée et le format.

   **Ce qui a été livré n'est pas forcément ce que la personne a choisi** :
   un film livré porte aussi les demandes du client. Écris « livré », jamais
   « tu préfères ».
3. **Ce que disent les rushes**, sans attendre la transcription : `describe`
   (durée, résolution, orientation, cadence et transfert de couleur de
   chaque rush, sous `library`), `media_sheet`
   (une image par rush : qui est filmé, où, combien de personnes). Un brief
   qui demande un 9:16 sur des rushes en 16:9 demande un cadrage ; une
   heure de parole pour une minute demande un choix de propos.

## Ce que tu écris : ce que tu as compris

Six points au plus, une ligne chacun, chacun avec sa source entre
parenthèses : `(brief)`, `(série : …)`, `(rushes)` ou `(hypothèse)`.

- **Les films** : combien, pour quelle marque et quelle série, pour où
  (réseau, salon, site).
- **Le format et la durée.**
- **Ce que le film doit faire** : faire comprendre ou faire faire quoi, en
  une phrase. Pour une interview, ce n'est pas encore le propos de l'étape
  5, qui se lit dans la parole ; c'est ce que la commande en attend.
- **Le ton et le rythme.**
- **Ce qui est imposé** : charte, police des légendes, nom à l'écran, logo,
  musique, mentions, appel à l'action.
- **Les livrables** : fichiers, versions (formats, avec ou sans sous-titres),
  timeline Resolve, SRT ; l'échéance si elle est dite.

## Les questions : deux ou trois, pas plus

Ne pose que celles dont la réponse change le film : si la réponse était
l'autre, tu monterais un autre film. Ne demande pas ce que le brief dit, ce
que la série montre, ce que les rushes montrent, ni ce que la procédure
fixe déjà par défaut (la sonie, l'étalonnage de base, le resserrement) :
cela va dans les hypothèses.

Chaque question porte l'hypothèse que tu prendras sans réponse, et se
répond d'un mot : « Je pars sur une minute en 9:16, comme les quatre films
livrés de la série (54 à 77 s). Plus court ? »

Pour une interview, celles qui coûtent une version quand on ne les pose
pas : quel propos porte le film quand la personne en développe deux ;
sous-titres incrustés, en SRT, ou repris dans Resolve. Un reel ou du motion
a les siennes, dans sa procédure.

## Ensuite

- **Quelqu'un répond** (une conversation) : attends sa réponse avant la
  première opération qui monte. Lance pendant ce temps ce qui est long et
  ne dépend pas de la réponse — la transcription, `analyze_speech_onset`,
  `align_words`, en tâche (`async: true`) : l'attente ne coûte rien. « Vas-y »
  vaut accord sur tes hypothèses.
- **Personne ne répond** (un lot, une série lancée pour la nuit) : écris tes
  hypothèses et avance. Elles vont dans le compte rendu et dans le fichier
  d'état, sous `brief`.
- **Une série** : le brief se précise une fois pour toute la série, par
  qui lance les agents ; chaque agent reçoit dans sa consigne les réponses
  ou les hypothèses ([serie.md](serie.md)). Quatre agents qui posent
  chacun leurs trois questions, c'est douze questions pour une seule
  réponse.
- **À la livraison**, nomme en tête du compte rendu les hypothèses restées
  sans réponse : c'est ce que la personne doit vérifier en premier.

## La trace : `brief` dans le compte rendu

Le bloc JSON qui termine le compte rendu d'une interview
([livraison.md](livraison.md)) porte le brief précisé. Un reel ou du motion
n'a pas ce bloc : recopie la même forme en tête de son compte rendu.

```json
"brief": {
  "compris": [
    {"point": "format et durée", "texte": "9:16, une minute environ",
     "source": "série : quatre films livrés, 54 à 77 s"}
  ],
  "questions": [
    {"question": "Plus court qu'une minute ?",
     "hypothese": "une minute", "reponse": null}
  ]
}
```

- `compris` : les points écrits plus haut, dans le même ordre ; `source`
  vaut `brief`, `rushes`, `hypothèse`, ou `série : …` avec ce qui a été lu.
- `questions` : chaque question posée, avec l'hypothèse prise par défaut ;
  `reponse` recopie la réponse telle qu'elle a été donnée, ou `null` si
  personne n'a répondu.
- Pas de brief à préciser (une retouche) : `"brief": null`.
