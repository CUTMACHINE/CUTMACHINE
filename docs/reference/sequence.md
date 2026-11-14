# Séquence

> Relu contre le code le 3 octobre 2026. Ce qu'on voit au moniteur (bandes,
> vue SDR, rendu des couleurs) n'est pas vérifié par lecture.

La timeline possède un objet `sequence` indépendant des médias sources, avec
un ULID, un nom, une largeur, une hauteur et une cadence rationnelle. Cet objet
possède directement ses `tracks` et ses `markers` : aucune timeline ne flotte
au niveau du projet sans séquence. Comme dans Premiere Pro ou Resolve, ce
canevas pilote le moniteur et sert de base aux
presets d’export ; une vidéo portrait ne transforme donc pas implicitement une
séquence horizontale. **Timeline → Réglages de séquence…** propose les formats
HD, UHD, vertical et carré ainsi que les cadences usuelles. Les documents plus
anciens sans bloc `sequence` ou antérieurs à la version 4 sont refusés.

L’application charge désormais un `Project` complet autour de ce document
actif. Toutes ses timelines apparaissent dans le chutier ; un double-clic en
active une et `Cmd+Option+N` en crée une nouvelle. Une autosave du projet
complet protège chaque commit atomique et peut être récupérée au lancement.

`--describe` écrit uniquement la vue JSON condensée sur stdout, avec les blocs
distincts `timeline` et `library`. Les médias de bibliothèque ont des alias
`M1`, `M2`, etc. et restent disponibles lorsqu'ils sont montés (`in_use:true`).
`--ingest` lit les en-têtes FFmpeg sans décoder l'image (seul le son est décodé,
pour mesurer son niveau moyen), conserve les cadences rationnelles
exactes, conserve la rotation de la display matrix et en déduit l'orientation
affichée. Le player reprobe aussi les sources montées au lancement : le bandeau
d'information expose codec, dimensions, orientation, rotation, cadence et
présence audio. Les vidéos portrait sont tournées puis ajustées au moniteur
avec leur ratio conservé et des bandes neutres, sans étirement. Ce cache de
présentation ne mute pas le document depuis l'interface. Les fichiers qui ne
sont pas des médias, ou corrompus, sont rapportés dans `errors` sans faire
échouer le lot ; l'identité
idempotente est le chemin absolu résolu.

Le schéma courant du document va de la version 7 à la version 19 (un plan
décrit, PLANS-2026-10 PL1 ; 18 pour une rampe de vitesse, REEL-2026-09 R3) :
un document
s'écrit à la plus ancienne qui porte tout ce qu'il contient, et les versions 4
à 6 restent lues. `library`, `bins` et `sources`
sont des objets projet ; `tracks` et `markers` appartiennent à `sequence`.
Les versions 1 à 3 sont refusées. Bibliothèque et source partagent l'ULID du
média ; l'ingest seul ne monte jamais de clip.

## Gestion colorimétrique

Le menu **Couleur** propose un preset direct **Sony S-Log3 → Rec.2020 HLG**
et un panneau avancé. La chaîne est persistée dans `color_management` au
niveau du projet : gamut et courbe d'entrée, matrice YCbCr, plage Full/Legal,
espace de grading, puis gamut et courbe de sortie. Le preset utilise la plage
Full imposée par la spécification Sony, S-Gamut3.Cine/S-Log3, ACEScct/AP1 comme
espace wide gamut de grading et une sortie Rec.2020/HLG. La plage et la matrice
peuvent aussi suivre automatiquement les métadonnées FFmpeg de chaque frame.

Chaque frame YUV planaire 8 à 16 bits est d'abord normalisée selon sa profondeur
et sa plage. OpenColorIO 2.5.2 applique ensuite les transformations de la
configuration intégrée et figée
`studio-config-v2.2.0_aces-v1.3_ocio-v2.4` : aucune variable `$OCIO` ni aucun
fichier externe n'intervient dans le rendu. Le player échantillonne les
processeurs OCIO en deux LUT 3D 65³ mises en cache par Metal, avant et après le
grading ACEScct ; la composition des pistes reste en AP1 linéaire dans une
texture flottante 16 bits. L'export utilise le même processeur OCIO sous forme
de LUT `.cube`. Les moniteurs de montage dérivent une vue locale Rec.709/SDR du
réglage de livraison afin que macOS ne présente pas le master HLG comme une
couche EDR au milieu de l'interface. Cette préférence d'affichage ne modifie ni
le document ni le rendu exporté.

Une livraison SDR passe par la vue « ACES 1.0 - SDR Video » de la
configuration. Une livraison HLG ne passe pas par la vue HDR d'ACES, qui
mettait le gris 18 % à 30 % du signal (environ 15 nits). Depuis le
20 octobre 2026, elle suit la courbe que DaVinci Resolve rend avec les
réglages de l'auteur (BLANC-HLG-2026-10 BH3, `HlgDelivery.h`) :
- un pied dans les noirs ;
- le gris 18 % à 37 % du signal, comme BT.2408 ;
- une épaule qui atteint le pic vers 17 fois le blanc.

Cette courbe est appliquée canal par canal en DaVinci Wide Gamut, puis
encodée en HLG avec le blanc de référence à 75 % du signal.

Pour juger un étalonnage, le menu **Couleur** propose aussi d'ignorer
l'étalonnage au moniteur Record (Maj+D : plans sans leurs étages `color.*`,
séquence sans son look) et de le comparer à une image de référence prise sur
Record (Cmd+Alt+G), montrée sur la moitié gauche du cadre (Cmd+Alt+W). Ce
sont des préférences locales du moniteur : ni le document, ni l'export, ni
les mesures ne changent.

Les réglages passent par l'opération réversible `SetColorManagement`, disponible
également via `--apply-op` et l'outil MCP `set_color_management`. Une combinaison
gamut/courbe d'entrée absente de la configuration intégrée est refusée avant le
rendu plutôt que remplacée silencieusement.

L'entrée `hlg` accepte les rushes déjà tournés en Rec.2020 HLG (courbe
ARIB STD-B67), comme ceux d'une caméra réglée en HLG plutôt qu'en S-Log3. Elle
n'est valide qu'avec le gamut `rec2020`.

Chaque média peut déclarer sa propre entrée, qui remplace celle du projet pour
lui seul : une vidéo de téléphone en Rec.709 ou en HLG dans un projet S-Log3
est lue dans son espace, au moniteur, à l'export, dans `read_frame`, les
planches et `measure_color`. L'ingest la déclare quand le fichier la dit : les
métadonnées d'une caméra Sony (S-Log3 / S-Gamut3.Cine, piste `rtmd`), sinon
les balises du conteneur (HLG en BT.2020, ou BT.709), jamais des balises
qu'une courbe maison annoncée par le conteneur contredit (un Blackmagic
Pocket 4K balise son Film log en BT.709). Un média qui ne déclare rien suit le
projet. L'opération réversible `SetMediaColorInput`, l'outil MCP
`set_media_color_input` (`from_file:true` relit les fichiers ingérés avant) et
le menu contextuel **Espace couleur d’entrée** du navigateur de médias la
règlent ; `describe` la liste en `color_input`. Un document qui en porte une
passe en version 16.

Un média peut aussi être déclaré **graphisme** (DEC31, 24 octobre 2026) :
un logo, un packshot, une image fixe. Ses valeurs de code sont alors ce
qu'il montre, comme les couleurs d'un titre de motion : sous gestion
couleur, il est posé dans la vue des graphismes et y est mélangé
(`OpenColorIoCompositeDisplayReferred`), au lieu d'être lu par l'entrée
d'une caméra ; un blanc sort blanc en SDR et au pic d'une livraison HLG.
Sans gestion couleur, seule change la matrice d'un graphisme RVB (un PNG),
convertie en BT.709. `set_media_color_input` avec `graphics: true` le
déclare (`input_ycbcr_matrix` et `input_range` disent, au besoin, comment
lire un graphisme YCbCr), comme le menu **Espace couleur d’entrée ›
Graphisme (logo, image)** ; l'ingest ne le devine jamais. Sous gestion
couleur, un graphisme ne prend pas d'étalonnage : l'export refuse un plan
graphisme qui porte un étage `color.*`, `measure_color`, `match_color` et
`read_frame` étalonné le refusent en le nommant, `propose_exposure` et
`propose_grade` le laissent de côté, et la planche étalonnée le montre tel
qu'il est écrit, sans le look de séquence. `conform_color_management` ne le
compte pas. Un document qui en porte un passe en version 21.

`conform_color_management` (MCP) règle l'entrée du projet sur ce que
déclarent les médias, chacun par sa propre déclaration, sinon par ses
balises : HLG ou S-Log3 active la gestion avec cette entrée, Rec.709 pose
l'entrée `rec709` sans toucher à l'activation. Des médias qui ne déclarent
rien ne décident rien : l'outil le dit et ne change rien, et c'est
`set_color_management` qu'il faut employer. Les réglages de travail et de
sortie sont conservés ; des balises mélangées sont listées dans
`declarations` et l'entrée la plus fréquente l'emporte. `preview:true` rend
la proposition sans l'appliquer.

`--apply-op`
réutilise le format canonique de `SerializeOperation`, remplace le document de
façon transactionnelle et conserve le journal dans le fichier compagnon
`<document>.timeline-<timeline-id>.editlog.json`, un par timeline. En cas de
refus, ni le document ni ce journal ne sont modifiés.

`--apply-project-op` applique de la même façon les opérations multi-timeline,
de métadonnées de chutier et de relink. Son historique distinct est conservé
dans `<document>.project-editlog.json` et les commandes `--undo-project-op` et
`--redo-project-op` le parcourent sans initialiser l’interface graphique.

Les objets passés à `--apply-project-op` doivent reprendre exactement l’ordre
et tous les champs produits par `SerializeProjectOperation`. `exact_project_hex`
vaut `null` pour une nouvelle intention ; le journal le remplace par
`exact_project_delta`, le delta exact de ce que l'opération a changé, pour un
rejeu exact. Les journaux plus anciens portent encore un projet complet sous
`exact_project_hex` et restent lus. Depuis le 24 octobre 2026 (DEC24), un tel journal est réécrit
sous la forme delta dès que l'éditeur ou `open_project` ouvre le projet,
sous le verrou de session, par la transaction du paquet ; le reste du paquet
n'est pas touché. Seul un changement qu'aucun delta n'exprime (un
réordonnancement) garde un projet complet. Exemples canoniques complets (les
identifiants sont des ULID) :

```json
{"type":"AddProjectTimeline","name":"Vertical","width":1080,"height":1920,"frame_rate":{"num":25,"den":1},"timeline_id":"","video_track_id":"","audio_track_id":"","exact_project_hex":null}
{"type":"CreateProjectTimelineFromSegments","name":"Extrait","width":1920,"height":1080,"frame_rate":{"num":25,"den":1},"segments":[{"source_id":"01K00000000000000000000001","source_in":{"value":0,"rate":25},"duration":{"value":50,"rate":25},"video_clip_id":"","audio_clip_id":"","link_group_id":""}],"timeline_id":"","video_track_id":"","audio_track_id":"","make_active":true,"exact_project_hex":null}
{"type":"RemoveProjectTimeline","timeline_id":"01K00000000000000000000002","exact_project_hex":null}
{"type":"SetProjectBinMetadata","item_id":"01K00000000000000000000001","metadata":{"description":"Interview retenue","rating":5,"tags":["interview"],"insert_order":0,"display_name":"Interview principale"},"exact_project_hex":null}
{"type":"SetProjectTimelineBin","timeline_id":"01K00000000000000000000002","bin_id":"01K00000000000000000000003","exact_project_hex":null}
{"type":"RenameProjectItem","item_id":"01K00000000000000000000002","name":"Montage vertical","exact_project_hex":null}
{"type":"SetActiveProjectTimeline","timeline_id":"01K00000000000000000000002","exact_project_hex":null}
{"type":"ChooseProjectTimeline","timeline_id":"01K00000000000000000000002","alternative_timeline_ids":["01K00000000000000000000004"],"make_active":false,"exact_project_hex":null}
{"type":"RelinkProjectMedia","replacements":[{"media_id":"01K00000000000000000000001","replacement":{"id":"01K00000000000000000000001","path":"rushes/interview.mov","filename":"interview.mov","codec":"h264","width":1920,"height":1080,"pixel_format":"yuv420p","color_range":"tv","color_space":"bt709","color_transfer":"bt709","color_primaries":"bt709","rotation_degrees":0,"rate":{"num":25,"den":1},"duration":{"value":250,"rate":25},"orientation":"landscape","has_video":1,"has_audio":1,"audio_rate":48000,"audio_channels":2,"bin_id":"","proxy_path":"","metadata_complete":1},"stored_path":"rushes/interview.mov"}],"exact_project_hex":null}
```
