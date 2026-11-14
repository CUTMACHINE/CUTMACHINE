# Référence de CUTMACHINE

La référence du moteur, un fichier par section. Ce qu'est le projet, ce
qu'il faut pour le compiler et un premier parcours à la ligne de commande
sont dans le [`LISEZMOI.md`](../../LISEZMOI.md) de la racine, qui porte la même
table.

- [Lancement](lancement.md) : ouvrir l'application, la boucle de
  développement, [quel FFmpeg](lancement.md#quel-ffmpeg) le moteur lance, le stockage et
  la collecte d'un projet.
- [Décisions de temps](decisions-de-temps.md) : ce que le temps exact impose
  au scrub, au playhead et à la composition des pistes.
- [Timeline graphique](timeline-graphique.md), [Audio](audio.md) et
  [Chutiers](chutiers.md) : la fenêtre.
- [API d'édition](api-edition.md) : les opérations et les outils de montage,
  les réglages d'un plan, et le [catalogue des outils par famille](api-edition.md#le-catalogue-par-famille).
- [Commandes headless](commandes-headless.md) : la ligne de commande, le
  serveur MCP, les interventions et les plans d'édition. `cutmachine --help`
  y renvoie.
- [Archive SQL des projets](archive-projets.md) : conserver les JSON complets
  à la sauvegarde, importer une collection et interroger avec SQL/JSON, au
  CLI ou par MCP.
- [Étapes durables et mesures d’agent](orchestration-montage.md) : reçus
  d’étapes sans double édition, captures liées, légendes dans un plan et
  rapport des traces ACP du panneau Agent.
- [Import depuis DaVinci Resolve](resolve.md) et
  [Format de séquence](format-de-sequence.md).
- [Transcription verbatim et nettoyage des
  hésitations](transcription.md) : le
  modèle Whisper, les mots, les silences.
- [Contrôle qualité des plans](controle-qualite.md),
  [Regarder un plan](regarder.md#regarder-un-plan),
  [Regarder le montage en planches](regarder.md#regarder-le-montage-en-planches),
  [Regarder un raccord tel qu'il est livré](regarder.md#regarder-un-raccord-tel-quil-est-livré),
  [Visionner les raccords](regarder.md#visionner-les-raccords),
  [Décrire et chercher des plans](regarder.md#décrire-les-plans-une-fois-les-chercher-ensuite) et
  [Mesurer l'étalonnage](mesurer-etalonnage.md) : ce que le moteur mesure et
  montre.
- [EDL CMX 3600 et WebVTT](edl.md) : les listes d'une timeline et de ses
  marqueurs, la lecture des marqueurs, les sous-titres WebVTT.
- [OTIO et FCPXML](echange.md) : une timeline écrite pour un autre
  éditeur, toutes ses pistes dans un fichier.
- [Export vidéo final](export.md) : l'export, sa vérification, les
  légendes, la mesure du son.
- [Séquence](sequence.md) : le document, la gestion colorimétrique, les
  opérations projet.
- [Agent intégré et clé utilisateur](agent-integre.md) et
  [Sidecar conversationnel](sidecar.md) : le pilotage par modèle.
- [Génération payante](generation.md) : proposer puis générer avec fal.ai,
  sur la clé de la personne et seulement à sa demande ; la provenance.
- [Images de référence dans les tests](tests-golden.md) : comparer un rendu
  CPU à une image PNG, avec sa tolérance, et réécrire les références.
- [Licence](../../LISEZMOI.md#licence).
