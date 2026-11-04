# Dolphin pour le launcher Wii Party

Ce dépôt est un fork de [Dolphin](https://github.com/dolphin-emu/dolphin) (GPL-2.0-or-later).
La branche `wp/main` part d'une version publiée de Dolphin et y ajoute uniquement ce qu'il faut
pour la distribuer avec le launcher Wii Party :

- `.github/workflows/wp-build.yml` : compile Linux x86_64, Windows x64 et macOS arm64 ;
  un tag `wp-*` publie une release avec les archives et `SHA256SUMS.txt`.
- `Tools/wp/package-linux.sh` : rend le build Linux portable (bibliothèques et plugins Qt inclus,
  `Sys/` à côté de l'exécutable), sans AppImage ni Flatpak.

Options de compilation : mise à jour automatique et statistiques désactivées
(`ENABLE_AUTOUPDATE=OFF`, `ENABLE_ANALYTICS=OFF`), distributeur `wii-party-launcher`.
Le code de Dolphin lui-même n'est pas modifié pour l'instant.

## Contenu des archives

| OS | Archive | Exécutable |
| --- | --- | --- |
| Linux | `dolphin-wp-linux-x86_64.tar.gz` | `usr/bin/dolphin-emu` |
| Windows | `dolphin-wp-windows-x64.zip` | `Dolphin/Dolphin.exe` |
| macOS | `dolphin-wp-macos-arm64.tar.gz` | `DolphinQt.app/Contents/MacOS/Dolphin` |

Le launcher lance toujours Dolphin avec `-u <dossier>` pour garder son propre profil.

## Publier une version

```sh
git tag wp-2609a-1   # <version Dolphin>-<révision Wii Party>
git push origin wp-2609a-1
```

Puis reporter les URL et les sommes de `SHA256SUMS.txt` dans `src-tauri/deps.json` du launcher.

## Construire Linux en local

```sh
git submodule update --init --recursive
cmake -S . -B build/wp-linux -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
  -DENABLE_AUTOUPDATE=OFF -DENABLE_ANALYTICS=OFF -DENABLE_TESTS=OFF -DDISTRIBUTOR=wii-party-launcher
cmake --build build/wp-linux
Tools/wp/package-linux.sh build/wp-linux dist/dolphin-wp-linux-x86_64.tar.gz
```
