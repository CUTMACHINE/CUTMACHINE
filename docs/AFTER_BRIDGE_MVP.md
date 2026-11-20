# Pont After Effects — inspection et édition de compositions

> Relu contre le code le 3 octobre 2026 : les outils et leurs arguments
> contre le catalogue (`./build/cutmachine --tools`), les schémas et les noms
> d'erreur contre `src/motion/AfterBridge.cc` et `tools/after-bridge/scripts/`.
> Non vérifié : ce que fait After Effects lui-même, aucune instance n'ayant
> été lancée pour cette relecture.

## Décision

Le pont pilote l'instance locale d'After Effects depuis le CLI, le serveur MCP
et le panneau conversationnel de CUTMACHINE. Il sait ouvrir une copie de
travail d'un `.aep`, inspecter ses compositions et modifier un sous-ensemble
explicite de leurs calques.

Le fichier After reste un espace de travail externe. Une opération After ne
lit ni ne mute un `Document` CUTMACHINE et n'entre donc pas dans `EditLog`.
Chaque mutation forme en revanche un groupe d'annulation natif dans After,
retourne un état `before`/`after` et enregistre le `.aep` par défaut.

Le trajet est unique :

```text
CLI --after-request / client MCP / chat intégré
  -> McpToolRegistry (pour MCP et le chat)
  -> McpBackend
  -> AfterBridge
  -> Apple Events / DoScript
  -> handler ExtendScript
  -> boîte aux lettres JSON privée et éphémère
  -> validation after-response.v2
```

## Pourquoi une boîte aux lettres

Le dictionnaire AppleScript d'After annonce un résultat texte pour `DoScript`,
mais After Effects 26 renvoie en pratique la valeur scalaire `1`, y compris
quand le script retourne une chaîne. Utiliser cette valeur rendait le MVP
incapable de parser sa propre réponse.

Le pont crée désormais un répertoire temporaire privé par appel. Le JSX écrit
la réponse dans un fichier temporaire puis le renomme atomiquement ; le C++ la
lit après l'acquittement Apple Event et détruit la boîte aux lettres. Il n'y a
ni fichier permanent, ni socket, ni démon, ni trafic réseau.

Cette voie exige le réglage After **Réglages > Scripts et expressions >
Autoriser les scripts à écrire des fichiers et à accéder au réseau**. Le pont
ne modifie jamais cette préférence utilisateur. Si elle est désactivée,
l'appel échoue avec `AfterFileAccessDenied` et une consigne actionnable.

## Sécurité des projets

`open_project_copy` ne donne jamais le projet source à modifier directement.
Le C++ :

1. exige deux chemins `.aep` absolus et distincts ;
2. exige que la source existe et que la cible n'existe pas ;
3. copie la source octet pour octet vers la cible ;
4. demande à After d'ouvrir uniquement cette copie ;
5. refuse de fermer un projet After déjà nommé ou non vide.

Le même refus d'écrasement s'applique à `create_project`. `save_project`
n'autorise pas un « Save As » silencieux d'un projet déjà lié à un autre
fichier.

## Capacités exposées

Aucun de ces outils ne demande de projet CUTMACHINE ouvert : ils adressent un
`.aep` et ne lisent ni n'écrivent aucun document (MOTION-AFTER-2026-09 MA10).

| Outil MCP | Effet |
|---|---|
| `after_create_project` | crée un `.aep` vide sans écrasement |
| `after_open_project_copy` | crée et ouvre une copie de travail |
| `after_describe_project` | décrit le projet actif |
| `after_list_comps` | liste les compositions et leurs identifiants hôte |
| `after_list_footage` | liste les métrages fichiers et signale ceux hors ligne |
| `after_describe_comp` | résume les calques, puis détaille transforms, effets et keyframes d'un calque demandé |
| `after_render_frame` | écrit une frame en PNG pour regarder le résultat |
| `after_set_text` | remplace le texte en conservant le `TextDocument` et son style |
| `after_set_property` | modifie une propriété de transform/effet, statique ou à une frame |
| `after_set_layer_state` | modifie visibilité, verrouillage, début, entrée ou sortie |
| `after_replace_footage` | remplace le fichier d'un métrage existant |
| `after_import_footage` | importe un fichier média comme métrage unique |
| `after_add_layer` | place un métrage ou une comp comme nouveau calque |
| `after_duplicate_layer` | copie un calque avec son animation intacte |
| `after_save_project` | enregistre le projet actif |
| `export_motion_after` | construit une composition de motion en projet modifiable (MOTION-AFTER-2026-09 MA1) |
| `verify_after_export` | fait rendre des images à After par `render_frame`, les compare au moteur et classe chaque écart : dans la tolérance, annoncé par le bilan de l'export, ou inexpliqué (MA2) |
| `import_after_comp` | lit une composition entière par la méthode `read_comp` (`after-comp.v1`, lecture seule) et l'ajoute au projet CUTMACHINE ouvert, un nœud par calque, ce qui n'est pas traduit dans `notes` (MOTION-BIBLIOTHEQUE-2026-10 MB7) ; `--import-after-comp` rejoue une lecture enregistrée |

Les mêmes requêtes strictes sont accessibles sans projet CUTMACHINE :

```sh
./build/cutmachine --after-request \
  '{"schema":"after-request.v2","method":"list_comps","arguments":{}}'
```

`McpProjectBackend` et `McpLiveBackend` appellent le même `AfterBridge`. Le
panneau conversationnel peut donc utiliser ces outils comme le serveur MCP
headless, sans deuxième implémentation.

## Regarder plutôt que déduire

Un arbre de propriétés ne dit pas à quoi ressemble une composition. Un média
hors ligne en est la démonstration : After le rend en mire SMPTE au lieu
d'échouer, donc un appelant qui ne lit que des propriétés voit un projet
parfaitement valide et livre des barres de couleur.

`after_render_frame` ferme cette boucle. Il écrit une frame en PNG avec
`CompItem.saveFrameToPng`, ce qui ne touche jamais la file d'attente de rendu
de l'utilisateur ni ses modules de sortie. Trois précautions le rendent
utilisable comme référence :

- la résolution de la composition est épinglée à 1:1 le temps du rendu puis
  remise exactement où elle était, sinon une comp laissée en demi-résolution
  renverrait silencieusement une image plus petite que celle décrite ;
- `saveFrameToPng` publie le fichier avant d'y écrire les octets, et un objet
  `File` ExtendScript garde en cache ce qu'il a appris du chemin. Le handler
  attend donc le marqueur de fin de flux `IEND` sur une poignée neuve plutôt
  que de se fier à `exists` — sans quoi un fichier vide passe pour un rendu
  réussi, ce qui est pire que pas de rendu du tout ;
- une cible existante n'est jamais écrasée. Numéroter les frames est ce qui
  rend une comparaison avant/après possible.

`after_list_footage` est le pendant d'inspection de `after_replace_footage` :
on pouvait réparer un métrage hors ligne sans pouvoir le trouver.

## Adressage et temps

Les items et calques sont adressés par `Item.id` et `Layer.id` rapportés par
After, jamais par leur position dans une collection. Une propriété est
adressée par un tableau de segments `matchName` (préféré) ou de noms affichés,
retourné par `after_describe_comp`. Une ambiguïté ou un segment absent est
refusé.

Sans `layer_id`, `after_describe_comp` ne renvoie que les résumés nécessaires
pour choisir un calque. Un second appel avec `layer_id` expose ses propriétés ;
ce découpage évite qu'un effet riche comme Lumetri sature inutilement le
contexte du modèle.

Les entrées temporelles sont des numéros de frame entiers. Le JSX convertit la
frame avec le `frameDuration` de la composition et vérifie le round-trip avant
d'écrire. Les flottants de l'API After restent à cette frontière externe et
ne deviennent jamais des temps CUTMACHINE.

Pour une propriété déjà animée, `after_set_property` exige `frame`. Sans ce
champ, le pont refuse de remplacer implicitement toute une animation. Avec ce
champ, il crée ou remplace la keyframe à la frame exacte.

## Transaction locale

Chaque mutation :

- inspecte et capture la valeur précédente ;
- ouvre un groupe `app.beginUndoGroup` ;
- applique une seule intention ;
- enregistre le projet, sauf si `save:false` est explicite ;
- restaure la valeur capturée si l'application ou l'enregistrement échoue ;
- retourne les valeurs `before` et `after`.

Le groupe reste annulable manuellement dans After. Il n'existe volontairement
pas d'undo distribué entre After et `EditLog`, car ce sont deux vérités et deux
historiques distincts.

## Limites actuelles

- macOS et une seule instance/projet After actif ;
- aucune création/suppression de composition, et aucun calque de texte, de
  forme ou de solide créé de zéro : un nouveau calque vient d'un item importé
  (`after_import_footage` puis `after_add_layer`) ou d'une copie
  (`after_duplicate_layer`) ;
- aucun ajout/suppression d'effet ; seuls les paramètres déjà présents sont
  modifiables ;
- aucune expression, interpolation ou édition de masque/forme ;
- aucune observation des changements manuels faits dans After : un état relu
  plus tôt puis réappliqué écrase sans avertissement ce qui a été changé à la
  main entre-temps (constaté le 13 septembre 2026). Depuis le 29 septembre,
  `after_set_text`, `after_set_property` et `after_set_layer_state` prennent
  `expected`, la valeur lue en dernier : si After tient autre chose, rien
  n'est écrit et le refus `StaleValue` donne la valeur trouvée (vérifié
  contre After 26 le 30 septembre). Sans `expected`, l'écriture écrase
  encore ; les autres mutations (métrage, calque ajouté ou dupliqué) n'ont
  pas d'écriture conditionnelle ;
- dans un projet construit par `export_motion_after`, la position a ses
  dimensions séparées : `ADBE Position` est masquée et refuse l'écriture,
  `ADBE Position_0` et `ADBE Position_1` la prennent ;
- les identifiants sont ceux du projet After, pas des ULID CUTMACHINE ;
- pas de rendu de séquence ni d'`aerender` : `after_render_frame` écrit une
  frame isolée, pas un média ;
- les dialogues modaux After peuvent encore provoquer un délai dépassé.

## Vérification

`tests/after_bridge_tests.cc` couvre les schémas v2, leurs round-trips, la
validation stricte, la séparation données/source JSX, la création réelle
d'une copie de travail, le refus d'écraser une image existante ou d'accepter
un format qu'After n'écrit pas, et la publication des quinze outils MCP. Le runner est
injecté pour que la suite reste déterministe et ne lance pas After.
`tests/after_jsx_tests.js` (`cutmachine_after_jsx_tests`) exécute les vrais
handlers d'écriture sous JavaScriptCore, contre un faux modèle d'After :
il vérifie ce que le handler décide (refuser une valeur qui a bougé, ne rien
écrire, n'ouvrir aucun groupe d'annulation), pas ce qu'After fait.

Le test bout en bout sur une vraie installation doit utiliser une copie de
projet jetable. Il vérifie dans l'ordre : ouverture de la copie, inventaire,
description d'une comp, mutation avec `save:false`, lecture du `after`, puis
restauration explicite ou annulation. Un test ne doit jamais viser le `.aep`
source.
