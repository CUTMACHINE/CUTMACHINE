# Stockage, portabilité et partage des projets

> Relu contre le code le 3 octobre 2026 (`src/ProjectStorage.cc`,
> `src/DocumentFormat.cc`, `src/Cli.cc`, `src/LookLutStorage.h`). Non
> vérifié : la page de démarrage et l’avertissement d’intégrité tels que la
> fenêtre les présente, et les trois critères non cochés de la section 7.

Statut : **spécification technique normative — format neuf v2 uniquement**.

## 1. Format accepté

CUTMACHINE ouvre uniquement un package Finder `<nom>.cutmachine-project` au
format v2. Les anciens fichiers JSON autonomes, documents timeline v1/v2,
enveloppes projet v1 et packages v1 sont refusés : aucune migration implicite
n’est exécutée.

```text
Film.cutmachine-project/
├── manifest.json
├── project.cutmachine.json
├── Timelines/
│   └── <timeline-ulid>.json
├── project.cutmachine.json.timeline-<timeline-ulid>.editlog.json
├── project.cutmachine.json.project-editlog.json
├── Media/                         # présent après une collecte
├── Luts/                          # look d’une séquence : <sha256>.cube (R6)
└── .cutmachine/                   # caches locaux régénérables
```

`manifest.json` porte `format: "cutmachine-collection"`, `version: 2`, le
chemin du projet, l’index des timelines et l’inventaire des médias collectés.
`project.cutmachine.json` porte `project_format: "cutmachine-project"` et
`project_version: 3` : la gestion couleur, la bibliothèque, les bins, les
sources et, s’il y en a, les compositions motion (`document_version` 8 au
moins, MOTION-2026-09 M3) y figurent une seule fois, suivis de `timelines` ;
`document_version` est la plus ancienne version de document qui porte ce que
le projet contient, de 7 à 17 ; chaque partie est
écrite et relue par le code de `Document` (`DocumentJson.h`). Un fichier
`project_version: 2`, qui embarquait un document complet par timeline dans
`timeline_snapshots`, reste lu et devient une version 3 à la sauvegarde
suivante (S7). Les fichiers `Timelines/<ULID>.json` sont
l’autorité pour le contenu des séquences ; les timelines du fichier projet
servent à la cohérence et à la récupération transactionnelle.

## 2. Autorité des données

| Élément | Autorité | Requis à l’ouverture | Régénérable |
|---|---:|---:|---:|
| `manifest.json` | oui | oui | non |
| `project.cutmachine.json` | oui | oui | non |
| `Timelines/<ULID>.json` | oui | oui | non |
| médias originaux | oui pour le rendu | non | non |
| journaux `.editlog.json` | historique undo/redo | non | non |
| `.autosave` | secours transitoire | non | non |
| `.cutmachine/` | non | non | oui |

Chaque timeline et son journal sont adressés par ULID. Le chargement vérifie que
l’ULID du fichier physique correspond à l’index, recompose le projet, puis le
valide. Perdre un journal ne détruit pas le montage mais supprime son historique.

Chaque entrée du journal projet porte, sous `exact_project_delta`, le delta
exact de ce que l’opération a changé (`ProjectDelta.h`) : sa taille suit le
changement, pas le projet. Les journaux écrits avant S6 portent sous
`exact_project_hex` deux projets complets encodés en hexadécimal ; ils restent
lus, chaque entrée est convertie en delta à la lecture, et la sauvegarde
suivante écrit la forme compacte.

## 3. Création et ouverture

La page de démarrage crée directement un package v2 avec une timeline. Elle
n’écrit jamais de projet JSON isolé. L’utilisateur ouvre le package depuis le
Finder ou la page d’accueil ; en interne, l’application adresse
`project.cutmachine.json`.

Les commandes headless reçoivent ce chemin interne :

```sh
cutmachine --describe Film.cutmachine-project/project.cutmachine.json
```

La liste des projets récents et les raccourcis clavier restent dans
`NSUserDefaults` et ne sont pas partagés avec le package.

## 4. Médias et chemins

Un média garde un ULID stable. Un chemin relatif est résolu depuis la racine du
package ; un chemin absolu reste local à la machine. Les caches, proxies,
waveforms et vignettes sont dérivés et peuvent être supprimés.

Un package de travail peut référencer des originaux externes. Pour le partager,
**Fichier → Collecter le projet…** crée un nouveau package autonome sans modifier
la source. La collecte :

- refuse d’écraser une destination existante ;
- exige tous les originaux lisibles ;
- copie chaque média sous `Media/<ULID>-<nom>` ;
- remplace les chemins par des chemins relatifs internes sans `..` ;
- exclut les proxies et caches ;
- copie sous `Luts/` le look de chaque séquence qui en porte un ;
- vérifie son empreinte et son format avant et après la copie ;
- emporte les fichiers `src` des compositions motion (images, vidéos, sons),
  y compris les choix stockés dans les props, versions, formats, clips,
  instances et répétitions ;
- conserve les journaux, réécrits comme l'état : annuler et rétablir dans la
  copie nomment ses propres fichiers, et ce que l'histoire seule nomme (un
  logo remplacé depuis) est copié s'il se lit (FIABILITE-2026-10 FI12) ;
  chaque journal réécrit est rejoué à côté de celui du source avant la
  publication, et un journal qui ne se prouve pas est copié tel quel et
  signalé ; une version de rush relinkée depuis n'est pas copiée ;
- inscrit taille et SHA-256 dans le manifeste ;
- recharge et valide la copie avant publication par renommage.

À l’ouverture, les tailles et SHA-256 sont contrôlés. Un original manquant ou
modifié déclenche un avertissement. Les empreintes vérifient le contenu mais ne
remplacent jamais les ULID métier. Une LUT absente ou dont les octets ne
correspondent plus à l'empreinte de la séquence est refusée. Réimporter le
fichier original répare la copie endommagée par remplacement atomique ; une
copie intacte n'est pas réécrite.

Les ressources motion relatives gardent leur chemin et leurs liaisons.
Un `src` littéral externe (absolu ou avec `..`), ou une sortie de table
locale à ce `src`, est copié sous `Media/Motion/<SHA-256><extension>` et
référencé par ce chemin dans la copie. Il en va de même d'une prop de texte
que sa composition ne lit que comme `src` entier d'un nœud, comme `logo`
dans le modèle livré : ses valeurs externes (défaut, versions, formats,
clips) sont réécrites dans la copie, et la liaison reste éditable. Une prop
lue ailleurs (un texte, une table, les props d'une instance), une valeur
transmise par une instance ou une table de tokens partagée qui nomme un
chemin externe est refusée : la remplacer en silence changerait aussi ses
autres usages. Donner un chemin relatif au package, ou nommer directement
le fichier dans `src`, permet la collecte. Une ressource manquante parmi
les choix stockés refuse toute la publication, sauf une valeur par défaut
qu'aucun clip ni aucune instance ne montre : elle est copiée si elle
désigne un fichier, laissée telle quelle sinon. Les empreintes du manifeste
portent sur les rushes ; les ressources motion relatives ne reçoivent pas
d'empreinte supplémentaire.

## 5. Sauvegarde, autosave et concurrence

Une sauvegarde prépare des fichiers temporaires voisins, inscrit la
transaction dans un marqueur `.cutmachine-transaction`, sauvegarde les
versions remplacées, puis publie ceux des fichiers du projet, des timelines
et des journaux dont les octets ont changé : un fichier déjà identique sur le
disque n’est pas réécrit (PERF6). Un rollback est tenté lorsqu’une étape
retourne une erreur, et une transaction interrompue est défaite à l’ouverture
suivante, à partir de son marqueur (S2).
L’autosave contient un snapshot canonique et peut être récupéré explicitement au
prochain lancement. Une sauvegarde automatique valide est aussi proposée si
le fichier principal est illisible, même si sa date est plus récente ; un
principal valide garde la priorité sur un autosave plus ancien ou de même date.

Un verrou `<project.cutmachine.json>.lock/owner` contient un jeton, le Mac,
l’utilisateur et le PID. Une seconde écriture est refusée. Un verrou dont le
PID local n’existe plus est retiré ; un verrou d’une autre machine n’est jamais
cassé automatiquement.

La transaction ne touche que les fichiers qu'elle a elle-même écrits : dans
`Timelines/`, un fichier n'est reconnu comme timeline obsolète que si son nom
est un ULID valide suivi de `.json`. C'est ce qui rend le format utilisable sur
un volume exFAT ou FAT — celui de la plupart des disques média partagés — où
macOS crée un fichier compagnon AppleDouble `._<nom>` à côté de chaque fichier.
Ces compagnons ne peuvent pas être renommés seuls, et les prendre pour des
timelines périmées faisait échouer toute sauvegarde d'un projet stocké sur un
tel disque.

Les renommages multiples ne sont pas une transaction matérielle face à un crash
machine. Il faut fermer CUTMACHINE et attendre la synchronisation complète avant
d’ouvrir un package sur un autre Mac. Pour un transfert, une archive ZIP fermée
est préférable à la synchronisation d’un projet actif.

## 6. Procédure de partage

1. Choisir **Fichier → Collecter le projet…**.
2. Attendre la validation finale.
3. Fermer le projet.
4. Copier le package ou le compresser en ZIP.
5. Sur l’autre Mac, décompresser puis ouvrir le `.cutmachine-project`.

Une archive pérenne doit aussi conserver un master vidéo indépendant de
CUTMACHINE et tout élément externe nécessaire au workflow. Les caches et
proxies ne remplacent jamais les originaux.

## 7. Critères d’acceptation

- [x] création et ouverture limitées au package v2 ;
- [x] rejet explicite des formats projet/timeline historiques ;
- [x] une timeline physique et un journal par ULID ;
- [x] collecte autonome avec chemins internes et SHA-256 ;
- [x] validation d’intégrité à l’ouverture ;
- [x] détection d’une seconde session d’écriture ;
- [ ] crash injecté à chaque étape laissant toujours une génération ouvrable :
      états d’arrêt simulés en test (S2) et CLI tuée 423 fois sur exFAT le
      24 septembre 2026 sans défaut après correction du verrou ; l’éditeur
      n’a pas été tué ;
- [ ] comparaison automatisée du rendu source et du rendu collecté ;
- [ ] transfert ZIP aller-retour testé sur deux machines.
