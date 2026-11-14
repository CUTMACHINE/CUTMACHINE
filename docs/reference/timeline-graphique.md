# Timeline graphique

> Relu contre le code le 3 octobre 2026 ; le mode trim, le Trim Monitor, le
> slide, le rate stretch et l'aimant des points In/Out ajoutés le 19 octobre
> 2026 (FILMCRAFT-2026-10, FC-fenêtre). Les gestes à la souris et le rendu
> ne sont pas vérifiés à l'écran : les couleurs nommées ici sont celles des
> jetons de `src/UiTheme.h` que `timelineRenderData` emploie.

La timeline est dessinée sous les moniteurs, dans son propre `CAMetalLayer`.
Les pistes, clips visibles, trous, bordure de sélection, aperçu de trim, graduation et
playhead sont des primitives Metal ; seul le bandeau d'information sous la
surface est un label AppKit. Le hit-test travaille sur les rectangles calculés
par `TimelineViewport`, avec une zone de bord fixe de 6 points.

Un drag de bord ne modifie pas le document pendant le geste. Il construit un
aperçu, valide un `TrimClipOperation` sur une copie, puis émet au plus une
opération via `EditLog::Apply` au relâchement. Un aperçu invalide est rouge et
n'émet rien. `Cmd+Z` et `Cmd+Shift+Z` utilisent le même journal que le CLI.
`Shift` pendant le drag active le ripple trim : le bord est retaillé et tous
les clips aval des pistes en sync lock suivent le raccord. `Cmd` active le roll
edit sur deux clips contigus : la coupe se déplace, mais la fin du programme ne
change pas. Avec l'outil Trim (`T`), le haut du corps d'un clip glisse son
contenu source sous un rectangle dont la position et la durée timeline restent
fixes (slip) ; le geste est borné par le début et la fin du rush. Les variantes
liées appliquent le même delta exact aux rectangles image et audio séparés ;
chacune est journalisée comme une seule opération annulable.

Depuis le 31 octobre 2026 (remarque de l'auteur), chaque trim s'arrête là où le
premier de ses plans s'épuise, comme le bord de Resolve s'arrête à la
dernière image du média : le trim simple au rush, au voisin et à une image de
long, comme avant ; le ripple et le roll au rush et à une image, sans voisin
puisqu'ils déplacent ce qui l'entoure ; un trim lié au plus court de ses
membres. Ils allaient au rouge et n'écrivaient rien au-delà. Un ripple
n'emporte que les plans liés de son propre groupe : un autre plan
sélectionné le faisait refuser en entier. Avec l'outil Trim, le curseur dit
le geste avant la pression : la double flèche sur un bord (ripple, roll), la
main ouverte sur le haut du corps (slip), le doigt sur sa barre de nom
(slide) ; il était la double flèche partout.
`Cmd+C` copie la sélection et `Cmd+V` la colle en overwrite au playhead ;
`Option`-glisser duplique directement vers la position visée. Une sélection
liée recrée des clips image/audio séparés avec un nouveau groupe de liaison,
dans une unique `PasteClipsOperation` annulable.
Le corps d'un clip est déplaçable horizontalement ou vers une autre piste de
même type. `MoveClipOperation` conserve l'ULID, les temps source et la durée ;
une destination incompatible refuse le drop. Un chevauchement effectue un
overwrite non-ripple : les clips entièrement couverts disparaissent, les
intersections de bord sont retaillées et un clip traversé est séparé en deux
survivants. Les états exacts des pistes affectées sont journalisés pour que
l'undo/redo restaure les ULID et les représentations rationnelles à l'octet.
Les trims sont bornés avant affichage par le début de timeline, les limites de
la source, une durée minimale d'un tick et les clips voisins : la poignée ne
peut donc jamais traverser une limite valide. Les raccords proches s'aimantent
dans une zone fixe de 8 points et affichent un guide orange ; `N` active ou coupe
ce magnétisme. Il prend les bords des clips de toutes les pistes, les
marqueurs, 0, et les points In et Out de la timeline (`SetInOutSnapTargets`,
FC1), sauf les clips que le geste emporte. Le clip sélectionné est entouré
d'une bordure claire d'épaisseur fixe, indépendante du zoom.

Le survol (curseur, bandeau d'information) et le clic lisent la même cible,
`TimelineHitAt` (`src/TimelineView.h`, FC2) : bande de zoom, drapeau de
marqueur, corps ou bord de clip, mode de trim, duplication, bascule de
sélection. Le menu contextuel garde son propre test.

Avec l'outil Sélection, un drag démarré dans une zone vide des pistes trace un
lasso orange dans la surface Metal. Après un seuil de 4 points, tous les clips
dont le rectangle intersecte le lasso sont sélectionnés, y compris sur
plusieurs pistes ; leurs bordures restent marquées au relâchement. En dessous
du seuil, le geste reste un simple clic de déplacement du playhead. Une
sélection de clips sans relation se déplace et se supprime d'un bloc
(`MoveClipsOperation`, `ClearClipsOperation`) ; `Shift`+clic sur le corps d'un
clip l'ajoute à la sélection ou l'en retire. `Cmd`+clic aussi, comme dans
Resolve (DEC40, 24 octobre 2026), à une nuance près : `Cmd` tenu dès
l'appui fait de la tête de lecture une cible de l'aimant pour le glisser qui
suit, si bien qu'un `Cmd`+clic sur un clip déjà sélectionné ne l'en retire
qu'au relâché, s'il n'a rien déplacé ; glissé, il emmène la sélection sur la
tête de lecture. `Option`+`Cmd` reste la duplication, `Cmd` sur un bord le
roll. Pour un groupe
A/V lié, move, trim et suppression utilisent chacun une opération multi-clip
atomique afin qu'un geste ne puisse pas être partiellement appliqué.

Un clic dans un trou borné sélectionne sa plage exacte et l'affiche en orange.
`Delete` ou `Backspace` raccorde alors la piste en décalant tous ses clips
suivants vers la gauche, sans déplacer les autres pistes. Cette fermeture de
trou est une unique `DeleteGapOperation` persistée dans l'event log ; elle est
donc annulable et rejouable à l'octet près. Lorsque **Sélection liée** est
active et que le clip suivant possède un audio lié, la fermeture du trou
décale ensemble les pistes image et audio de la même durée exacte.

La palette en haut à gauche expose les outils Sélection (`A`), Main (`H`),
Zoom (`Ctrl+Z`, la touche Contrôle, avec `Option` pour dézoomer), Lame (`B`)
et Trim (`T`). La lame affiche
la future coupe en orange et un clic dans un clip crée deux segments contigus,
avec un nouvel ULID stable pour celui de droite. Lorsque **Sélection liée** est
active, la même coupe est appliquée atomiquement au clip audio séparé et les
deux côtés deviennent deux nouvelles paires A/V indépendantes. La sélection permet aussi de
scrubber en continu dans les trous et la règle. `Espace` lance ou arrête la lecture ;
maintenu pendant un drag, il devient temporairement l'outil Main. `J`, `K` et
`L` contrôlent la lecture arrière, l'arrêt et la lecture avant. `Shift+Z` cadre
toute la timeline, `Cmd+=`/`Cmd+-` (ou `+`/`-` seuls) zooment autour de la tête
de lecture, Home/End rejoignent les extrémités, les flèches gauche/droite
avancent d'une frame (`Shift` : une seconde de timecode), et haut/bas vont au
raccord précédent/suivant. Maintenir `Cmd` pendant le scrub aimante la playhead
au raccord le plus proche ; pendant un move ou un trim, `Cmd` ajoute aussi la
playhead aux cibles de magnétisme.
Un média glissé depuis le panneau Médias se monte comme dans Resolve (manuel
21.1, p. 891-893) : en overwrite, `Cmd+Shift` pour une insertion, `Option`
pour l'image seule et `Shift` pour le son seul. L'image va sur la piste vidéo
visée et le son sur la piste audio du même numéro (lâché sur V2, le son va en
A2), ou l'inverse quand le pointeur est sur une piste audio. Une piste est
créée là où il n'y en a pas pour le recevoir : dans le vide sous la dernière
piste ou sur une piste de légendes (une piste de chaque sorte que le média
porte), quand le numéro manque à l'autre sorte, ou quand la piste partenaire
est verrouillée. Une piste verrouillée sous le pointeur refuse ce qu'elle
devrait recevoir. Un média sans image (une musique) se monte sur une piste
audio seule. Le dépôt, ses pistes créées comprises, est un seul pas
d'annulation (`TimelineDropTracksAt`, `TimelineView.h`). Il n'y a plus de
ligne `+` sous les en-têtes : le menu Timeline, le menu contextuel de la
timeline, `Cmd+Shift+T` (piste vidéo) et `Cmd+Option+Shift+T` (piste audio)
ajoutent une piste vide. La création est atomique et
annulable ; un clip peut ensuite être glissé vers une piste compatible avec
l'outil Sélection. Le player résout et compose toutes les pistes vidéo du
document, sans limite fonctionnelle fixée à deux couches. `Cmd+T` ajoute un
fondu centré sur le cut au playhead (ou sur un raccord voisin du clip
sélectionné) : un fondu enchaîné sur une piste vidéo, un fondu croisé sur une
piste son (EFFETS-2026-10 EF2). La transition est un objet de séquence
distinct des clips : elle référence les deux clips, valide les poignées média
exactes, se journalise via `Add/Update/RemoveTransition` et se rend de façon
identique dans Metal et dans l’export FFmpeg. Elle ne crée ni ne modifie
aucun clip.
`D` (menu **Clip > Activer / désactiver le plan**, ou clic droit sur un
clip) est le Enable/Disable Clip de Resolve, demandé par l'auteur le 29 octobre
2026 : sur toute la sélection, si un clip au moins est actif, tous sont
désactivés, sinon tous sont réactivés, en une `SetClipEnabledOperation` et
une annulation. Un clip d'une piste verrouillée garde son état. Un clip
désactivé reste à sa place, grisé (corps sans teinte, forme d'onde et nom
éteints, `kClipDisabled` dans `UiTheme.h`), et n'est ni vu ni entendu à la
lecture ni à l'export ([API d'édition](api-edition.md), « Réglages d'un
plan »). Avec la sélection liée, un clic prend l'image et son son : `D` les
désactive ensemble. Ce rendu n'a pas été vu à l'écran.

L'onglet **Effets** du panneau de gauche (EF1) liste les effets par
catégorie : les transitions vidéo (fondu enchaîné, fondu au noir, fondu au
blanc), les transitions audio (fondu croisé +3 dB) et les effets vidéo (flou
gaussien, EF3). Une ligne se glisse sur la timeline. Une transition vise le
raccord le plus proche du plan sous le pointeur, surligné pendant le glissé,
et le dépôt pose une transition d'une seconde centrée, raccourcie aux
poignées disponibles, ou change le type de celle qui y est déjà ; seuls les
fondus au noir et au blanc se posent au bord d'une piste. Un effet vidéo vise
le plan entier sous le pointeur, surligné d'un bord à l'autre : le flou
gaussien lui donne un flou d'un cent-huitième du petit côté de l'image
(10 px sur une séquence 1080p, horizontale ou verticale), que la ligne
**Flou** de l'Inspecteur (section Composition, 0 à 100 px) règle ensuite ;
redéposé sur un plan déjà flouté, il est refusé et renvoie à l'Inspecteur.
Un plan de motion, un plan son et un plan dont le cadrage est animé ne
prennent pas de flou ; la raison s'affiche dans la barre d'état. Un
double-clic dans la liste pose une transition au raccord le plus proche de la
tête de lecture, un effet vidéo sur le plan sélectionné, ou à défaut sur le
plan vidéo du dessus sous la tête de lecture. Chaque dépôt est une seule
opération, une seule annulation.

Un clic droit sur une piste propose **Supprimer la piste** et **Supprimer les
pistes vides**. Une piste occupée demande confirmation ; ses clips sont inclus
dans l'inverse de `RemoveTrackOperation`, afin que `Cmd+Z` restaure la piste et
son contenu exacts.

Le menu **CUTMACHINE > Raccourcis clavier…** (`Cmd+,`) permet de réassigner les
outils, la lecture J/K/L, les points In/Out, delete/ripple delete, le fondu
enchaîné, le magnétisme, la sélection liée, le cadrage et les commandes Source
vers Record. Un clavier AZERTY visuel permet de choisir une commande, ses
modificateurs, puis de cliquer directement sur la touche à lui affecter. La
saisie textuelle reste disponible pour les combinaisons comme `A`, `Space`, `F9`,
`Alt+X` ou `Cmd+Shift+L`. Une valeur vide désactive la commande. Les doublons et les
conflits avec les raccourcis système fixes sont refusés ; un bouton restaure
les valeurs par défaut. La configuration est globale à l'application,
persistée dans les préférences macOS, et les menus sont actualisés
immédiatement. Le plein écran est affecté à `P` par défaut et reste
réassignable dans cette même fenêtre.

**Affichage → Sortie vidéo (clean feed)** affiche l'image de Record en
entier sur un autre écran HDMI/USB-C, en laissant l'éditeur utilisable.
L'écran de l'éditeur est grisé ; **Désactivée** arrête la sortie. Légendes,
motion et étalonnage restent visibles, les commandes, scopes, sélections,
zoom et comparaison de référence restent dans Record. En trim, la sortie
montre les deux images sans leurs compteurs. C'est le même aperçu Rec.709
SDR que Record. Le choix est une préférence locale ; une déconnexion suspend
la sortie et le rebranchement la rétablit. L'agent y accède par
`get_video_output` et `set_video_output`.

Les touches par défaut suivent celles de DaVinci Resolve 21.1 (préréglage
« DaVinci Resolve », relevé le 10 octobre 2026 par son API de scripting) :
Sélection `A`, Lame `B`, Trim `T`, Tout cadrer `Shift+Z`, Insérer `F9`,
Écraser `F10`. La touche Suppr. vaut
`Shift+Delete`, la suppression ripple, comme `Del` dans Resolve. Deux
commandes que Resolve n'a pas prennent une combinaison qu'il laisse libre :
l'outil Zoom (`Ctrl+Z`) et la bascule image/échantillon de la tête de lecture
(`Alt+Shift+M`). La table et sa vérification contre Resolve sont dans
`src/KeyboardShortcuts.h` et `tests/keyboard_shortcuts_tests.cc`. Une
personnalisation déjà enregistrée garde ses touches. L'ancien outil Slip
(`tool.slip`) est devenu l'outil Trim (`tool.trim`) le 19 octobre 2026 ;
du 19 au 24 octobre, une touche personnalisée sous l'ancien nom n'était plus
lue. Depuis le 24 octobre (DEC40), elle revient à l'outil Trim tant qu'aucune
touche n'est enregistrée sous `tool.trim` (`RenamedCommands`,
`StoredSpecFor`), et s'enregistre sous le nouveau nom au prochain
« Enregistrer » du panneau des raccourcis.

Une touche dont le caractère demande `Maj` se lit sans ce `Maj` : sur un
clavier français, le point est `Maj` et la touche `;`, et il vaut le point ;
`Maj+,` (`?` en AZERTY, `<` en QWERTY) vaut `Maj+,`. `Maj+.` ne se tape pas
en AZERTY : le gros pas en avant y passe par le menu **Trim** ou une autre
touche (`TypedKeyMatches`, `src/KeyboardShortcuts.h`).

## Mode trim et Trim Monitor

FILMCRAFT-2026-10, câblé dans la fenêtre le 19 octobre 2026. La politique est
celle de `src/TrimSession.h` (FC4) ; la fenêtre lit son état dedans et
applique ce qu'elle planifie, une opération par geste via `EditLog::Apply`,
donc une seule annulation par geste. Les touches sont celles du préréglage
par défaut de Resolve 21.1, page du manuel en regard :

| Touche | Commande | Manuel de Resolve 21.1 |
|---|---|---|
| `T` | Outil Trim : un bord se ripple (`Cmd` : roll), le haut du corps glisse son contenu (slip), sa barre de nom le glisse entre ses voisins (slide) | p. 1061, 1063-1066, 1069 |
| `A` | Outil Sélection : les mêmes gestes clavier redimensionnent au lieu de rippler | p. 1061-1062, 1069 |
| `V` | Le point de montage le plus proche de la tête de lecture, sur les pistes ciblées, en roll ; la tête y va | p. 1069 |
| `U` | Côté du point : sortie (A), roll, entrée (B), à tour de rôle | p. 1070 |
| `,` / `.` | Une image en arrière / en avant | p. 1069-1070 |
| `Maj+,` / `Maj+.` | Cinq images (le « fast nudge », 5 par défaut) | p. 1069-1070 |
| `E` | Les points sélectionnés à la tête de lecture (Extend Edit) | p. 1070, 1088-1089 |
| `W` | Trim dynamique : `J` et `L` trimment sur l'horloge au lieu de lire, `K` écrit le décalage atteint en une seule opération, `Échap` l'abandonne sans rien écrire | p. 1069, 1085-1086 |
| `Cmd+R` | Contrôles de vitesse du clip sélectionné ; un bout de leur bande, glissé, étire le clip (rate stretch), et ripple avec l'outil Trim | p. 1218-1219 |
| `Échap` | Abandonne un trim dynamique, sinon quitte le mode trim, et ferme les contrôles de vitesse | p. 1222 |
| `/` | Joue de 2 s avant à 2 s après le raccord pris, les plans sélectionnés ou la tête de lecture, puis y ramène la tête (ON9) | p. 1070 |
| `Cmd+/` | Lecture en boucle : `/` recommence au lieu de s'arrêter (ON9) | p. 1070 |

`V` prend un seul instant, le raccord le plus proche, sur chaque piste
ciblée qui en a un (`SelectNearestEdit`) : avec toutes les pistes ciblées,
le plus proche de chaque piste emportait une coupe de musique lointaine.
Un clic sur un bord qui ne déplace rien sélectionne son point (sortie pour
une fin de clip, entrée pour un début, roll avec `Cmd`) ; `Maj`-clic
l'ajoute à la sélection ou l'en retire. Les points sont marqués sur la zone
où un clic les prend (`VisibleTimelineEdgeMarks`) : vert tant qu'il reste du
média au-delà, rouge au bout du média, comme Resolve (p. 1089). Un clic
ailleurs dans les pistes, une annulation, un changement de timeline et
`Échap` quittent le mode ; la sélection liée fait suivre le son lié
(`FollowLinks`). Le menu **Trim** reprend ces commandes.

Depuis le 31 octobre 2026 (ON9), le centre d'une coupe entre deux plans
collés (la coupe et 2 points de part et d'autre) fait un roll sans `Cmd`,
avec l'outil Sélection comme avec l'outil Trim ; le curseur le dit, comme
dans Resolve : `]` sur la fin du plan sortant, `[` sur le début du plan
entrant, `][` sur la coupe qui roule. Un double-clic sur une coupe la prend
en roll. En mode trim, à l'arrêt, une pression sur le Trim Monitor prend A
(image de gauche), B (image de droite) ou un roll (la séparation, 24 points
de part et d'autre du milieu) ; le glissé déplace le point de six points par
image, en aperçu, et le relâché l'écrit en une annulation.

Depuis le 31 octobre 2026 (ON8), une barre dans la rangée d'outils de la
timeline montre le mode trim tant que l'outil Trim est l'outil ou qu'un point
est pris : sans point, **Prendre le raccord** (`V`) ; avec un point, l'état
(ripple ou simple, le nombre de points), **A · Roll · B** (le côté pris
allumé, un clic le choisit), **−5 −1 +1 +5**, **Étendre**, **Dynamique** et
**Quitter**. La bulle de chaque bouton dit sa touche ; les boutons font ce
que font leurs touches, sans prendre le clavier.

À l'arrêt et en mode trim, le moniteur Record devient le Trim Monitor : la
dernière image de A à gauche, la première de B à droite, chacune entière
dans sa moitié de la trame (`LayoutTrimMonitor`), avec au-dessus son côté et
les images trimmées depuis la sélection du point (en vert dès qu'elles
bougent), dessous ce que le trim peut encore prendre (`handle_frames`).
Rien que des rectangles du thème, du texte de l'atlas et les deux textures.
Les deux images viennent souvent d'un même rush : celle qui manque au cache
est redemandée jusqu'à ce que les deux soient là. La lecture rend Record au
programme.

Le slide (barre de nom, outil Trim) passe par `SlideEditOperation` : les
voisins absorbent le déplacement, et le son lié suit quand la sélection liée
est active, par la même règle que `slide_edit` du MCP
(`SlideClipsWithLinkedPartners`, `src/Operations.h`). Le rate stretch passe
par `RateStretchOperation` : le clip garde sa plage source, sa vitesse
devient exacte (plage / durée) ou le geste s'arrête au dernier nombre
d'images qui donne une vitesse exacte et ne chevauche rien. Avec l'outil
Sélection rien ne ripple ; avec l'outil Trim, depuis le 24 octobre 2026
(DEC40), l'étirement ripple comme dans Resolve (p. 1219) : le clip garde sa
tête, quel que soit le bout glissé, et ce qui le suit se décale de la
différence, sur ses pistes et sur les pistes synchronisées
(`RateStretchOperation::ripple`, `TimelineTrimMode::RippleRateStretch`). Ni
le slide ni le stretch ne s'aimantent.

Le cadenas dans chaque en-tête verrouille la piste via une opération persistée
et annulable. Une piste verrouillée reste rendue et lisible sous des hachures,
mais toute mutation qui la cible est refusée atomiquement. Le séparateur
vertical du chutier ajuste sa largeur ; la frontière entre moniteur et timeline
se glisse verticalement pour adapter l’espace de montage.
