# Format de séquence

> Relu contre le code le 3 octobre 2026.

Une séquence neuve est en 1920×1080 à 25 i/s. Ce n'est presque jamais le
format des rushes, et le corriger à la main demande de savoir une chose que
les fichiers cachent : **un tournage vertical est stocké en paysage** avec un
drapeau de rotation. Lire `3840×2160` dans les métadonnées et le recopier dans
la séquence donne un montage couché.

`--propose-sequence` déduit le format des rushes du projet, sans rien modifier :

```sh
./build/cutmachine --propose-sequence ./Film.cutmachine-project/project.cutmachine.json
```

```json
{"ok":true,
 "chosen":{"width":2160,"height":3840,"frame_rate":{"num":25,"den":1},"media_count":365},
 "unanimous":false,"media_considered":405,"media_ignored":0,
 "candidates":[{"width":2160,"height":3840,"frame_rate":{"num":25,"den":1},"media_count":365},
               {"width":2160,"height":3840,"frame_rate":{"num":50,"den":1},"media_count":40}]}
```

La règle est la majorité, et elle est entièrement déterministe : à égalité de
comptes c'est la plus grande image qui tranche, puis la cadence la plus haute,
puis les champs bruts — le même projet donne toujours la même réponse, quel que
soit l'ordre de la médiathèque. Les formats minoritaires sont **rapportés**, pas
escamotés : un tournage à cadences mixtes doit se voir. Les médias sans image
exploitable (son seul, rotation qui n'est pas un angle droit) sont comptés à
part plutôt que rattachés de force.

Côté agent, l'outil MCP `conform_sequence` applique ce format en une
`UpdateSequenceOperation` réversible, et retourne ce qu'il a choisi et ce qu'il
a écarté. `preview: true` calcule sans appliquer. Une séquence déjà conforme est
signalée sans être réécrite, pour ne pas encombrer l'historique d'une opération
nulle.

C'est la division du travail du principe 7 : l'appelant nomme l'intention
(« que la séquence corresponde aux rushes »), le moteur calcule les nombres.
`update_sequence` reste disponible pour poser un format arbitraire.
