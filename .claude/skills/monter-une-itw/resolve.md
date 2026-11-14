# Renvoyer dans DaVinci Resolve

## Sommaire

- [Avant de commencer](#avant-de-commencer)
- [Construire une timeline : les sous-titres d'abord](#construire-une-timeline--les-sous-titres-dabord)
- [La voix va sur une piste mono](#la-voix-va-sur-une-piste-mono)
- [Les bornes que Resolve rapporte](#les-bornes-que-resolve-rapporte)
- [Resolve met en cache le média importé, par son chemin](#resolve-met-en-cache-le-média-importé-par-son-chemin)
- [Les illustrations à une autre cadence](#les-illustrations-à-une-autre-cadence)
- [Reporter le style des sous-titres](#reporter-le-style-des-sous-titres)
- [Compléter les sous-titres d'une timeline déjà retouchée](#compléter-les-sous-titres-dune-timeline-déjà-retouchée)
- [Changer une illustration sur une timeline déjà étalonnée](#changer-une-illustration-sur-une-timeline-déjà-étalonnée)
- [Les contrôles après renvoi](#les-contrôles-après-renvoi)
- [La transcription de Resolve](#la-transcription-de-resolve)
- [Illustrer avec du matériel extérieur](#illustrer-avec-du-matériel-extérieur)

## Avant de commencer

- Le projet Resolve doit être **ouvert** : au lancement, Resolve part sur
  « Untitled Project » et le pont rapporte 0 chutier sans erreur.
- **« Remonte la timeline » : le livrable est la timeline renvoyée**, et le
  SRT si le projet en portait un ; le monteur étalonne et sort le master
  lui-même.
- Un primaire passe d'un plan à Resolve, ou en revient, par la CDL
  (`export_cdl`, `apply_cdl`).
- **Lis le README de scripting installé avant de chercher en ligne**
  (`/Library/Application Support/Blackmagic Design/DaVinci Resolve/Developer/Scripting/README.txt`) :
  il correspond à la version installée, la documentation publique non.

## Construire une timeline : les sous-titres d'abord

`AppendToTimeline` pose l'élément de sous-titres **à la fin de ce que la timeline
contient déjà**, pas au début de sa piste. Sur une timeline remplie, les cues
atterrissent après le dernier plan et la timeline double de longueur, sans
erreur. Construis dans cet ordre :

1. `CreateEmptyTimeline` ;
2. `AddTrack("subtitle")` ;
3. `AppendToTimeline` du SRT, qui tombe à zéro puisque la timeline est vide ;
4. `AddTrack("video")` autant que de couches ;
5. les plans, avec leur `recordFrame` **absolu** (décalé du `GetStartFrame()` de
   la timeline : 90000 pour 01:00:00:00 à 25 i/s).

Ne mets **rien d'autre** que `mediaPoolItem` et `trackIndex` dans le `clipInfo`
du SRT : y ajouter `startFrame`, `endFrame`, `recordFrame` ou `mediaType` fait
planter le pont.

**Dans le `clipInfo` d'un plan, `endFrame` est exclusif** : la durée vaut
`endFrame − startFrame`. Le réflexe `startFrame + n − 1` rend chaque plan une
image trop court ; sur une piste d'illustration bord à bord, l'interview du
dessous clignote à chaque raccord, et la durée totale ne bouge pas. Relis
`GetStart()` et `GetDuration()` de chaque plan posé et vérifie
`fin[i] == début[i+1]`.

**Il n'y a pas de contournement** (même `recordFrame` est ignoré, sans
erreur) : sous-titrer une timeline déjà montée demande de la reconstruire,
ou de passer par le `.drt` si le monteur l'a retouchée (voir plus bas). Pour
ne pas perdre son travail :

- **construis à côté, jamais par-dessus** : une timeline sœur (`…_ST`), l'autre
  intacte tant que le relevé ne coïncide pas ;
- **relève d'abord tout ce que la timeline porte**, pas seulement les plans. Sur
  un film, onze plans portaient une transformation faite à la main (quatre
  recadrages à 1,52× avec chacun son tilt, sept incrustations à 0,32×) : elles ne
  survivent qu'à un `GetProperty` avant et un `SetProperty` après. Vérifie aussi
  les marqueurs, les compositions Fusion et les versions d'étalonnage : ce qui
  n'est pas relevé est perdu sans avertissement.

## La voix va sur une piste mono

**Consigne du monteur : toute piste stéréo qui porte de la voix passe en mono.**

La voix des rushes mesurés est en double mono (L − R à −63 dB) : le mono ne
perd rien. Si les deux canaux diffèrent (micro-cravate d'un côté, micro
caméra de l'autre), demande lequel garder.

Ce que le scripting permet, mesuré en 20.3.1 :

- **`CreateEmptyTimeline` crée A1 en stéréo**, sans rien signaler ;
- `timeline.GetTrackSubType("audio", i)` lit le format (`"mono"`, `"stereo"`…),
  mais **aucune méthode ne le change** sur une piste existante ;
- `AddTrack("audio", "mono")` crée une piste mono, et `AppendToTimeline` avec ce
  `trackIndex` et `mediaType: 2` y pose un rush stéréo : la piste reste mono, le
  plan garde un mapping source `stereo` `[1, 2]`. Un append **lié** V+A visant
  une piste mono n'a pas été mesuré : relis `GetTrackSubType` après la pose ;
- dans le `.drt`, le format est le `<SubType>` de la `<Sm2TiTrack>` audio :
  **0 = stéréo, 1 = mono**. Ni le `<FieldsBlob>` de la piste ni le
  `<VirtualAudioTrackBA>` des plans ne changent. Une timeline importée avec
  `SubType` 1 se relit `mono`.

Donc : à la construction, crée la piste de voix en mono avant d'y poser les
plans. Sur une timeline déjà retouchée, passe `<SubType>` de 0 à 1 dans le `.drt`
de l'état actuel, réimporte à côté, contrôle, remplace — ou demande au monteur de
le faire dans l'interface, c'est un clic.

## Les bornes que Resolve rapporte

Appelé avec `startFrame: X`, `GetSourceStartFrame()` rend parfois **X − 1** :
42 plans sur 79 sur une reconstruction, sans motif visible (même rush, même
cadence, voisins conformes). Position et durée restent exactes, donc les coupes
ne bougent pas.

**Ne réinjecte pas l'écart.** L'écart n'est pas déterministe : demander `X + 1`
ne rend pas `X` de façon fiable. Après deux réinjections, 29 plans étaient à +1
et 13 à +2. Mesure toujours contre la borne d'origine, jamais contre la demande
courante. Si la première passe donne position et durée exactes, **arrête-toi
là** : c'est la version qui porte les bornes voulues.

## Resolve met en cache le média importé, par son chemin

**Réécrire un `.srt` sur le disque ne rafraîchit pas l'élément du Media Pool** :
`ImportMedia` rend l'ancien, et la timeline repart avec les cues de la version
précédente. Ça ne se voit pas dans le retour de l'API, et pas toujours dans la
durée : sur huit montages renvoyés, sept portaient l'ancien sous-titrage, et
seuls trois avaient une durée aberrante.

`resolve_bridge.py --send … --srt` s'en charge depuis le 18 septembre 2026 :

- il importe une **copie nommée d'après le contenu** du SRT
  (`<nom>-<empreinte>.srt`, dans
  `~/Library/Application Support/CUTMACHINE/resolve-srt/`, un chemin durable
  puisque Resolve garde le lien). Un SRT réécrit au même chemin part donc
  sous un nom neuf ; le même contenu garde le même nom ;
- il **compte les éléments de sous-titres** posés, sur toutes les pistes de
  sous-titres, et échoue (code de sortie 1, les deux nombres dans le message)
  s'ils diffèrent des cues du fichier. Aucun plan n'est alors posé, mais la
  timeline créée reste dans Resolve : supprime-la avant de renvoyer.

Hors du pont (un script qui appelle `ImportMedia` lui-même), les deux règles
restent à appliquer à la main : nom de fichier neuf à chaque révision, sur un
chemin durable, et comptage des éléments contre les cues.

## Les illustrations à une autre cadence

Stockshots et films tiers arrivent en 23,976 ou 24 i/s pour un montage à 25.
`--export-resolve-timeline` **refuse** ces plans, et il a raison : il exige une
durée qui tombe sur une image source entière, et aucune durée n'est entière à la
fois à 25 et à 23,976 (il faudrait 1001 images de séquence, 40 s).

Ne les fais pas porter par CUTMACHINE : monte la parole dans le moteur, exporte,
puis **écris la couche d'illustration directement dans le fichier d'échange**
avec `video_layer: 1` et `with_audio: false`. `path` doit être celui de l'élément
du Media Pool ; `send_timeline` ajoute la piste vidéo qu'il faut.

- **Resolve conforme par la durée** : N images source occupent
  `N × 25 / cadence_source` images de séquence (vérifié sur 137 plans).
- **Et il tronque, il n'arrondit pas** : la durée rendue vaut le plancher de ce
  produit. Dimensionner à l'arrondi laisse la coupe une image trop courte, donc
  une image d'interview qui clignote dessous. Choisis la longueur source dont le
  plancher vaut la durée voulue, et positionne la coupe suivante sur cette
  durée-là, pas sur celle que tu visais.
- Deux coupes contiguës posées sur les durées visées se chevauchent d'une
  demi-image, et Resolve écrase silencieusement.

## Reporter le style des sous-titres

Quand le monteur a stylé les cartons d'une timeline et demande ce look sur les
autres, aucune API ne le donne : `timelineItem.GetProperty()` sur un carton ne
rend que le bloc de transformation (Pan, Tilt, ZoomX…), identique entre une
timeline stylée et une timeline nue, et les 157 réglages de
`timeline.GetSetting()` sont identiques aussi.

**Le look est dans le `FieldsBlob` de la piste de sous-titres.** Une piste nue y
porte deux champs (`NumLayers`, `ExcludeTrackFromSequenceCaching`) ; une piste
stylée en porte un troisième, `EffectFiltersBA`, avec le descripteur de police Qt
en clair (`Fira Sans,13,-1,5,57,0` + `Medium` : ce 13 n'est pas la taille du
style) et le reste compressé en zstd, taille comprise (champ 19).

Le seul chemin est un aller-retour `.drt`, une archive zip contenant
`project.xml`, `MediaPool/Master/MpFolder.xml` et `SeqContainer/<uuid>.xml` :

1. `timeline.Export(chemin, resolve.EXPORT_DRT)` sur la timeline stylée, et
   relever le `<FieldsBlob>` qui suit `<SubtitleTrackVec>` ;
2. exporter chaque timeline à styler, y remplacer ce même blob, rezipper ;
3. `mediaPool.ImportTimelineFromFile(chemin)`.

- **Le nom de la timeline importée vient du nom du fichier `.drt`**, pas du XML.
- Le contenu survit intact (plans, couches, son, sous-titres, positions et durées
  relus à l'identique), sans doublon de média.
- La greffe se prouve en **réexportant la timeline importée et en comparant son
  blob** à celui de la source. Rien d'autre ne le montre, surtout pas l'API.

Le style maison se lit dans les projets récents du monteur. Prends-le sur
une piste que le monteur a réglée dans le même
projet, ou sur une piste active (`Flags` = 0) d'un projet récent du même
client, jamais sur une piste désactivée : un style copié apporte sa taille
et sa position.

Comme la timeline doit de toute façon être reconstruite pour changer les cues,
fais les deux d'un coup : reconstruis avec le nouveau SRT, puis greffe le blob à
l'import.

## Compléter les sous-titres d'une timeline déjà retouchée

Quand le monteur a rallongé un film après le sous-titrage, ne reconstruis pas la
timeline : ses retouches (cartons corrigés, coupes à la main, plans déplacés)
seraient perdues. Exporte le `.drt` **de l'état actuel**, ajoute les cartons
manquants dans S1 et S2, réimporte à côté, contrôle, remplace.

- Un carton neuf se fabrique depuis un carton existant sans mise en forme :
  nouveau `DbId`, `<Name>`, `<Start>`, `<Duration>`.
- Le varint du `<FieldsBlob>` d'un carton (`…a801` + varint + `1211…`) est la
  position qu'avait le carton **à sa création** : un carton déplacé ou
  copié-collé garde l'ancienne. N'importe quelle valeur passe ; prends la
  position neuve.
- Relève d'abord ce qui a bougé : un copier-coller avait laissé un carton en
  double par-dessus les mots nouveaux.
- Recale ensuite les frontières sur les cuts **de l'état relevé** (voir
  [sous-titres.md](sous-titres.md)) : les coupes du monteur en ont créé de
  nouveaux.

## Changer une illustration sur une timeline déjà étalonnée

Quand le monteur a étalonné et recadré, ne reconstruis rien et ne supprime
rien : l'étalonnage vit dans les plans de la timeline et dans leurs groupes
couleur, et aucune API ne le relit pour le reposer. Fait le 1er octobre 2026
sur huit plans, sans rien perdre :

- **relève d'abord** chaque plan des pistes vidéo (`GetProperty()` pour la
  géométrie, `GetNodeGraph().GetNumNodes()` et `GetToolsInNode`,
  `GetColorGroup()`), garde le relevé, et refais-le après pour prouver que
  V1 et V2 sont identiques ;
- **pose le remplaçant sur une piste vide au-dessus**, à la position et à la
  durée exactes du plan qu'il couvre (`AppendToTimeline` avec `mediaType: 1`,
  `trackIndex`, `recordFrame`). Le plan d'origine reste dessous avec son
  grade : désactiver la piste redonne le montage d'avant ;
- **donne-lui un étalonnage** : `donneur.CopyGrades([nouveau])` puis
  `nouveau.AssignToColorGroup(donneur.GetColorGroup())`. Le bon donneur est
  un plan déjà étalonné **de la même scène**, pas forcément le plan couvert
  (le grade d'un détail de maquette ne va pas à un visage) ;
- **regarde le résultat** : `SetCurrentTimecode`, 1,5 s d'attente, puis
  `project.ExportCurrentFrameAsStill(chemin.png)` rend l'image étalonnée
  depuis la page Edit. Note le timecode de départ et remets-le à la fin ;
- le script vérifie avant d'écrire que les plans visés sont encore où le
  relevé les a vus (le monteur travaille peut-être en même temps), enregistre
  le projet, et tourne sous minuterie.

## Les contrôles après renvoi

- `GetEndFrame() - GetStartFrame()` vaut **exactement** la durée du montage. Plus
  long : le piège de l'ordre des sous-titres.
- **La piste d'interview n'a aucun trou**, et la piste son compte autant de plans
  qu'elle. Un trou = une image noire.
- **Relis positions et durées de la piste d'illustration** telles que Resolve les
  rapporte, pas telles que tu les as demandées : la troncature de conformation et
  l'`endFrame` exclusif se voient là.
- Le nombre d'éléments de sous-titres vaut le nombre de cues du SRT. Sinon : le
  piège du cache. Le pont le vérifie désormais lui-même à l'envoi ; recompte
  après toute retouche faite hors du pont (`.drt`, second SRT).
- Le premier élément de sous-titre part de zéro, à une image ou deux près selon
  la première cue.
- **Chaque piste de voix se relit `mono`** par `GetTrackSubType`.
- **Aucun plan de parole sans mot aligné** sur le relevé (voir
  [resserrement-et-finition.md](resserrement-et-finition.md)).
- **Aucune frontière de carton à 1-12 images d'un cut** sur le relevé, hors cas
  refusés et listés.
- Resolve refuse un nom déjà pris : pour republier une révision, supprime
  l'ancienne timeline (`DeleteClips` sur son MediaPoolItem) ou change de nom.

## La transcription de Resolve

Elle ne sert plus de juge : MFA pose de meilleures frontières. Les
frontières que le monteur reprend à la main sont du goût, pas de la dérive :
ne cherche pas à le modéliser.

Pièges de `timeline.CreateSubtitlesFromAudio()`, mesurés en 20.3.1 :

- **elle échoue en silence hors de la page Edit** : depuis Deliver, elle rend
  `False` en zéro seconde. `resolve.OpenPage("edit")` d'abord ;
- **les clés de réglage ne sont pas des chaînes** : `{"AutoCaptionLanguage": 5}`
  rend `False`. Les clés sont les constantes `resolve.SUBTITLE_LANGUAGE`,
  `SUBTITLE_CAPTION_PRESET`, `SUBTITLE_CHARS_PER_LINE`, `SUBTITLE_LINE_BREAK`,
  `SUBTITLE_GAP` (0 à 4), documentées dans le README local (« Auto Caption
  Settings »), avec 1 à 60 caractères par ligne ;
- **mais le dictionnaire échoue en silence** : avec les bonnes clés, l'appel rend
  `True` et ne crée aucun carton, quelle que soit la valeur. Seul l'appel **sans
  argument** marche (42 caractères). `limitSubtitleCPL` et
  `transcriptionLanguage` s'écrivent par `SetSetting` sans rien changer ;
- **l'appel répété finit par bloquer le moteur** : même l'appel nu rend `True`
  avec zéro carton en une seconde au lieu de ~20 s. Rien ne le signale ; il faut
  redémarrer Resolve. Fais les essais de réglage en dernier.

## Illustrer avec du matériel extérieur

Quand le monteur fournit une vidéo tierce pour illustrer (film de projet, démo
d'application, rendus, captures d'écran), elle n'a ni la cadence, ni le format,
ni l'espace couleur des rushes.

### Dans le moteur ou dans Resolve

Une vidéo tierce se monte aussi dans le moteur (`set_clip_framing`,
`set_media_color_input`) ; Resolve reste le bon chemin quand le reste de
l'illustration s'y fait. Un plan posé dans Resolve n'existe que là : dis au
monteur que les livrables doivent alors sortir de Resolve.

### Resolve recadre déjà pour remplir : n'ajoute pas de zoom

Avec `timelineInputResMismatchBehavior = scaleToFit`, un 16/9 posé dans une
timeline 9/16 **remplit déjà la hauteur et recadre les côtés** à
`ZoomX/ZoomY = 1.0`. Appliquer « le zoom qu'il faut pour remplir » (1920/608 =
3,16) donne un sur-zoom d'un facteur trois. Vérifie toujours à `1.0` d'abord.
Pour une **capture d'écran d'interface**, c'est l'inverse : le recadrage plein
cadre rend le texte illisible, il faut *réduire* (zoom ~0,8, à ajuster en
regardant) ; le fond noir de l'interface masque les bandes.

### Choisis les plans sur un recadrage, pas sur l'image entière

Choisis les plans 16:9 sur leur recadrage 9:16 (`crop=ih*9/16:ih`), pas sur
l'image entière : sur une série, un tiers des plans retenus en entier ne
survivaient pas au recadrage.

### Les cadences non entières faussent le calcul d'images

Une source à 24, 23,976 ou en cadence variable ne se compte pas comme la
timeline. Les bornes se donnent en **images de la source**, calculées depuis des
secondes, et l'arrondi coûte parfois une image sur la durée : sans conséquence
sur un plan isolé, jamais acceptable entre deux plans jointifs (voir l'`endFrame`
exclusif plus haut).

Le cas le plus traître est la **cadence variable** (une capture d'écran à
120 i/s annoncée 60 par Resolve, 57,7 en moyenne réelle) : la numérotation
d'images de Resolve ne correspond plus à `secondes × cadence`, et le plan posé ne
joue pas ce qu'on visait. Il n'y a pas de calcul fiable : **pose, capture,
regarde, corrige**.

### `GrabStill` court après le rendu

`SetCurrentTimecode` puis `GrabStill` immédiatement rend **l'image précédente** :
Resolve n'a pas fini de rendre, deux captures consécutives sortent identiques, et
on croit à une erreur de montage. Laisse ~1 s entre les deux.
