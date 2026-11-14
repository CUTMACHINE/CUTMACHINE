# CLAUDE.md

CUTMACHINE est un éditeur vidéo que pilote un agent. Une session ouverte ici
fait l'une de deux choses.

- **Monter un film** : par les outils `mcp__cutmachine__*` (déclarés dans
  `.mcp.json`, une fois le binaire compilé), jamais par `--call-tool` dans un
  shell. Les skills `monter-une-itw`, `monter-un-reel` et `motion-design`
  disent comment.
- **Développer le moteur ou l'app** : [`LISEZMOI.md`](LISEZMOI.md) dit
  comment compiler et lancer les tests ; la référence du moteur est sous
  [`docs/reference/`](docs/reference/README.md).

Une clé d'API ne s'écrit jamais dans un fichier suivi.
