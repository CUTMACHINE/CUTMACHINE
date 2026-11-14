## Contexte

<!-- Pourquoi ce changement ? Quel problème il résout. -->

## Changements

<!-- Ce qui a été modifié, fichier par fichier si utile. -->

## Notes

<!-- Limitations connues, points à surveiller, décisions notables. -->

## Checklist

- [ ] `ctest --test-dir build --output-on-failure` passe sur le Mac (aucun job ne joue une PR sur macOS)
- [ ] `CTest` (Linux, sans fenêtre) passe
- [ ] `lint` (clang-format) passe ; clang-tidy lancé à la main sur les fichiers modifiés
- [ ] `secrets-scan` passe
- [ ] Pas de secret / credential dans le diff
