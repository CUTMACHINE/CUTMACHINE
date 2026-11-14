# Pont After Effects

Ce dossier contient les handlers ExtendScript du pont local After Effects.
L'orchestration, la validation et le transport vivent dans
`src/AfterBridge.{h,cc}`. Aucun script ne lit ou ne modifie un document
CUTMACHINE.

## Workflow recommandé

1. Activer dans After **Réglages > Scripts et expressions > Autoriser les
   scripts à écrire des fichiers et à accéder au réseau**.
2. Ouvrir une copie avec `after_open_project_copy` ; le pont refuse d'écraser
   une cible et ne modifie jamais la source.
3. Appeler `after_list_footage`. Un métrage hors ligne se rend en mire SMPTE
   sans lever d'erreur : tout ce qui suit serait jugé sur une image fausse.
4. Appeler `after_list_comps`, puis `after_describe_comp`.
5. Rendre une frame de référence avec `after_render_frame` avant de toucher
   à quoi que ce soit.
6. Modifier avec les IDs et chemins de propriété retournés.
7. Relire la comp, vérifier le `before`/`after` de chaque mutation, et rendre
   une nouvelle frame numérotée à comparer à la référence.

Une écriture de texte, de propriété ou d'état de calque prend `expected`, la
valeur lue au point 4 : si quelqu'un l'a changée à la main dans After
depuis, rien n'est écrit (`StaleValue`, avec la valeur trouvée). Relire,
demander, puis réécrire.

Les quinze outils sont `after_create_project`, `after_open_project_copy`,
`after_describe_project`, `after_list_comps`, `after_list_footage`,
`after_describe_comp`, `after_render_frame`, `after_set_text`,
`after_set_property`, `after_set_layer_state`, `after_replace_footage`,
`after_import_footage`, `after_add_layer`, `after_duplicate_layer` et
`after_save_project`. Ils n'ont pas besoin d'un projet CUTMACHINE ouvert : le
pont ne lit ni n'écrit aucun document (MA10, 9 octobre 2026).

Une composition de motion part vers After par `export_motion_after` (MA1) :
le moteur la décrit, `motion_import.jsx` la construit dans le projet vide
d'After et enregistre le `.aep`. Le même script se lance à la main
(**Fichier > Scripts > Exécuter le fichier de script**) sur une description
écrite par `cutmachine --export-motion-after`.

## CLI direct

Le CLI utilise exactement l'enveloppe du MCP :

```sh
./build/cutmachine --after-request \
  '{"schema":"after-request.v2","method":"list_comps","arguments":{}}'
```

Les exemples de `examples/` peuvent être passés après compactage sur une seule
ligne. Voir `protocol.md` pour le contrat complet.

## Configuration locale

Le pont cherche les installations sous `/Applications/Adobe After Effects *`
et choisit déterministement la plus récente. Deux variables permettent de
remplacer les chemins sans écrire de préférence dans le projet :

```sh
export CUTMACHINE_AFTER_EFFECTS_APP="/Applications/Adobe After Effects 2026/Adobe After Effects 2026.app"
export CUTMACHINE_AFTER_BRIDGE_SCRIPTS="$PWD/tools/after-bridge/scripts"
```

Le premier appel peut déclencher l'autorisation macOS permettant au processus
appelant de contrôler After Effects. Le pont ne contourne ni cette autorisation
ni les préférences After. Fermer les dialogues modaux avant un test automatisé.

## MCP headless

```sh
./build/cutmachine --mcp-serve \
  /chemin/Film.cutmachine-project/project.cutmachine.json --port 9876

curl -sS http://127.0.0.1:9876/mcp \
  -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"after_list_comps","arguments":{}}}'
```

Le panneau conversationnel passe par le même catalogue et le même pont via
`McpLiveBackend`.
