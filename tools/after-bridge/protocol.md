# Protocole du pont After Effects

## Transport

CUTMACHINE valide un objet `after-request.v2`, l'encode comme donnée dans un
job JSX et l'exécute avec l'Apple Event `DoScript`. After Effects 26 accusant
réception avec la valeur `1` au lieu du texte retourné par le script, le JSX
écrit `after-response.v2` dans une boîte aux lettres temporaire privée. Le
fichier est publié par renommage atomique, lu par le C++, puis supprimé.

Ce transport exige l'autorisation d'écriture des scripts dans les préférences
After. Il n'utilise ni réseau, ni service résident, ni fichier permanent.

## Requête commune

```json
{
  "schema": "after-request.v2",
  "method": "list_comps",
  "arguments": {}
}
```

Les trois champs sont obligatoires. `arguments` est toujours un objet et toute
clé inconnue est refusée.

| Méthode | Arguments |
|---|---|
| `create_project` | `project_path` absolu, cible `.aep` absente |
| `open_project_copy` | `source_project_path` existant et `working_project_path` absent |
| `describe_project` | aucun |
| `list_comps` | aucun |
| `list_footage` | aucun |
| `describe_comp` | `comp_id`, `layer_id?` pour les propriétés détaillées |
| `render_frame` | `comp_id`, `frame`, `image_path` absolu `.png` absent |
| `set_text` | `comp_id`, `layer_id`, `text`, `expected?`, `save?` |
| `set_property` | `comp_id`, `layer_id`, `property_path`, `value`, `frame?`, `expected?`, `save?` |
| `set_layer_state` | `comp_id`, `layer_id`, au moins un état, `expected?`, `save?` |
| `replace_footage` | `footage_id`, `media_path`, `save?` |
| `import_footage` | `media_path` existant, `folder_name?`, `save?` |
| `add_layer` | `comp_id`, `item_id`, `index?`, `start_frame?`, `in_frame?`, `out_frame?`, `save?` |
| `duplicate_layer` | `comp_id`, `layer_id`, `save?` |
| `save_project` | `project_path?` |
| `build_motion_project` | `description_path` absolu `.json` existant, `project_path` absolu `.aep` absent |

Les états acceptés par `set_layer_state` sont `enabled`, `locked`,
`start_frame`, `in_frame` et `out_frame`. Les frames sont des entiers exacts.
`property_path` est un tableau non vide de `matchName` ou noms retournés par
`describe_comp`. `value` est un scalaire JSON ou un tableau plat de scalaires.

`render_frame` est le seul moyen de voir le résultat d'une modification au
lieu de le déduire. Il n'écrase jamais une image existante : des frames
numérotées sont ce qui rend une comparaison avant/après possible. Il n'écrit
pas non plus dans la file d'attente de rendu.

`add_layer` place un item déjà présent dans le projet ; importer et placer
restent deux appels, pour qu'un placement raté ne laisse pas un item orphelin
et pour qu'un même média puisse être placé deux fois. `index` est une position
d'empilement à base 1, `1` étant le calque du dessus.

`build_motion_project` (MA1, `docs/roadmap/MOTION-AFTER-2026-09.md`)
construit une composition de motion décrite en `after-motion.v1`
(`src/MotionAfterExport.h`) dans le projet vide et sans titre qu'After a
ouvert, comme `create_project`, puis l'enregistre. Un projet qui contient du
travail est refusé (`ProjectAlreadyOpen`), jamais fermé ; une construction
qui échoue ferme sans l'enregistrer ce projet, qui ne contenait qu'elle. Le
handler est `motion_import.jsx`, le script qu'on lance aussi à la main.
`result` donne `main_comp_id`, `comps`, `layers` et `seconds`. L'outil MCP
`export_motion_after` décrit la composition puis appelle cette méthode.

`expected` rend une écriture conditionnelle : c'est la valeur que
l'appelant a lue en dernier (texte, valeur de la propriété à `frame` si elle
est donnée, ou objet des états de calque attendus). Si After tient autre
chose, le handler n'écrit rien, n'ouvre aucun groupe d'annulation et répond
`StaleValue`, avec la valeur trouvée dans `detail`. Les nombres se comparent
à un millionième de leur taille près, puisqu'ils ont traversé une écriture
décimale ; textes et booléens à l'identique. After lit la position d'un
calque 2D sur trois composantes (`[x, y, 0]`) et l'accepte sur deux : une
valeur attendue plus courte vaut la valeur lue si ce qu'elle omet vaut 0.

Les mutations enregistrent par défaut. `save:false` conserve la modification
en mémoire dans After et laisse l'appelant décider quand appeler
`save_project`.

## Réponse

```json
{
  "schema": "after-response.v2",
  "ok": true,
  "project_name": "Animation-test",
  "path": "/tmp/Animation-test.aep",
  "item_count": 12,
  "after_version": "26.0",
  "result": {}
}
```

`path` est obligatoire mais peut être vide pour un projet non enregistré.
`item_count`, `after_version` et l'objet `result` sont facultatifs. Les méthodes
d'inspection placent leurs collections dans `result`; les mutations y placent
les identifiants, la propriété éventuelle et les valeurs `before`/`after`.

Une réponse d'échec contient uniquement l'enveloppe et l'erreur :

```json
{
  "schema": "after-response.v2",
  "ok": false,
  "error": "UnknownComp",
  "detail": "La composition demandée est introuvable."
}
```

Codes hôte usuels : `NoProject`, `ProjectAlreadyOpen`, `ProjectExists`,
`WorkingCopyMissing`, `OpenFailed`, `UnknownComp`, `UnknownLayer`,
`UnknownItem`, `NotTextLayer`, `FrameRequired`, `FrameOutOfRange`,
`ExpressionActive`, `StaleValue`, `UnknownFootage`, `MediaMissing`,
`UnsupportedMedia`,
`InvalidDescription`,
`InvalidLayerSource`, `RenderUnavailable`, `RenderFailed`,
`ProjectPathRequired`, `ProjectPathMismatch`, `SaveFailed` et
`AfterScriptError`.

Le C++ peut échouer avant une réponse hôte avec `ValidationFailed`, `IoError`,
`AfterNotInstalled`, `AfterConfigurationError`, `AfterAutomationDenied`,
`AfterLaunchFailed`, `AfterTimeout`, `AfterExecutionFailed`,
`AfterFileAccessDenied` ou `AfterProtocolError`.

## Exemple d'édition

Après `list_comps` puis `describe_comp`, une position statique :

```json
{
  "schema": "after-request.v2",
  "method": "set_property",
  "arguments": {
    "comp_id": "42",
    "layer_id": "9",
    "property_path": ["ADBE Transform Group", "ADBE Position"],
    "value": [960, 540]
  }
}
```

Pour une propriété animée, ajouter `"frame": 120`. Un edit sans frame est
refusé si la propriété possède déjà des keyframes.

## Exemple de boucle visuelle

Un métrage hors ligne se rend en mire SMPTE sans lever d'erreur. La séquence
qui protège de ce piège commence donc par l'inventaire, garde une référence,
puis compare :

```json
{"schema":"after-request.v2","method":"list_footage","arguments":{}}
{"schema":"after-request.v2","method":"render_frame","arguments":{"comp_id":"413","frame":410,"image_path":"/tmp/ref-410.png"}}
{"schema":"after-request.v2","method":"set_text","arguments":{"comp_id":"413","layer_id":"421","text":"…","save":false}}
{"schema":"after-request.v2","method":"render_frame","arguments":{"comp_id":"413","frame":410,"image_path":"/tmp/apres-410.png"}}
```
