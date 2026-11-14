# OTIO et FCPXML

> PA1, PARITE-2026-11, 11 novembre 2026.

Une timeline s'écrit dans les deux formats que lisent les autres éditeurs :
OpenTimelineIO (`.otio`, un fichier JSON) et Final Cut Pro XML (`.fcpxml`,
version 1.10). Pour DaVinci Resolve, le montage entier part par
[`send_to_resolve`](resolve.md) (PA16), et un cadrage animé par
`export_resolve_xml` ; pour une liste par piste, [l'EDL](edl.md).

**Rien de ce que ces deux exports écrivent n'a été ouvert dans un éditeur**,
ni relu par la bibliothèque OpenTimelineIO : les fichiers suivent les formats
tels que leur documentation les décrit. Chaque rapport le dit
(`verified_in_editor: false`).

```sh
./build/cutmachine --export-otio ./Film.cutmachine-project/project.cutmachine.json \
  /chemin/film.otio --timeline '<timeline-id>'
./build/cutmachine --export-fcpxml ./Film.cutmachine-project/project.cutmachine.json \
  /chemin/film.fcpxml
```

MCP : `export_otio` et `export_fcpxml` (`path` absolu). Le fichier de
destination doit être nouveau : un fichier existant est refusé
(`OutputExists`), comme pour l'EDL et le XML de Resolve.

## Ce que les deux emportent

- toutes les pistes vidéo et son dans un seul fichier, avec les trous entre
  les plans ;
- la vitesse constante d'un plan, et l'arrêt sur image ;
- un plan désactivé, écrit désactivé ; un angle multicam caché et les plans
  d'une piste masquée ou muette voyagent désactivés eux aussi, et sont
  comptés (`multicam_angles_disabled`, `hidden_tracks_disabled`) ;
- les marqueurs de la timeline ;
- le chemin absolu de chaque rush : un chemin rangé relativement au paquet
  est résolu avant l'écriture.

Aucune position n'est refusée. Les deux formats écrivent un temps comme un
rationnel : en OTIO une valeur à une cadence, en FCPXML une fraction de
seconde. En OTIO, un temps qui tombe sur une image entière s'écrit en images
(celles de la séquence pour la timeline, celles du rush pour la source) ; un
temps à l'intérieur d'une image s'écrit dans sa propre cadence et se compte
dans `off_frame_times`.

## Ce qui diffère

| | OTIO | FCPXML |
|---|---|---|
| Disposition | une `Track.1` par piste, vidéo du bas vers le haut puis son | un `gap` de la longueur du film, chaque plan connecté dessus ; pistes vidéo sur les `lane` 1, 2…, pistes son sur −1, −2… |
| Vitesse | effet `LinearTimeWarp` (`time_scalar`) ; `FreezeFrame` pour un arrêt | `timeMap` à deux points ; `start` compté dans le temps du plan une fois ralenti |
| Fondu enchaîné, fondu croisé | `Transition.1` entre les deux plans, s'ils se touchent | non emporté |
| Fondu à une couleur, fondu au bord d'une piste | non emporté | non emporté |
| Couleur d'un marqueur | le mot le plus proche parmi les onze d'OTIO ; l'écart est nommé (`colors_approximated`), la couleur d'origine reste dans les métadonnées | non emportée |

En FCPXML, un plan d'une piste vidéo ne joue que l'image et un plan d'une
piste son que le son (`srcEnable`), comme dans le document.

## Ce qui reste derrière

Compté et nommé dans `losses`, par plan, piste ou séquence : l'étalonnage
(`grade`), le cadrage (`framing`), l'opacité (`opacity`), le flou (`blur`),
le gain et les fondus d'un plan son (`audio_gain`, `audio_fade`), une rampe
de vitesse, écrite à sa vitesse moyenne (`speed_ramp`), un plan de motion,
dont la place reste vide (`motion_clip`), une piste de légendes
(`caption_track`), le look de la séquence (`look_lut`), et chaque transition
que le format ne place pas (`transition`).

Refusé, par son nom : un plan dont le rush n'est pas dans la médiathèque,
une timeline sans plan, un chemin relatif ou d'une autre extension.
