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

Dans le code de Dolphin, trois additions :

- `Source/Core/Core/HW/WiimoteReal/IOBridge.*` : si `WP_WIIMOTE_BRIDGE` nomme une socket (Linux,
  macOS) ou un tube nommé (Windows, `\\.\pipe\...`), Dolphin y demande les Wiimotes de chaque place
  au launcher, qui reste connecté à elles, au lieu d'en chercher lui-même en Bluetooth. Même
  protocole partout ; les rapports envoyés au launcher commencent toujours par `0xA2`.
- `Source/Core/Core/WPGameState.*` : si `WP_GAME_STATE` est défini, Dolphin écrit sur stdout la scène,
  le mode et le mini-jeu de Wii Party dès qu'ils changent (présence Discord du launcher), et sur les
  plateaux de l'Île aux défis, de Globe-trotter et de Trios de Mii les nombres des joueurs (`wp-stats` :
  cases avant le défi final et volcan, pièces, photos et repos, points), lus dans le `.bss` du REL du plateau.
- `Source/Core/VideoCommon/WPOverlay.*` : si `WP_OVERLAY` est défini (un objet JSON : les couleurs du
  launcher, son logo, les textes de l'écran de chargement), la fenêtre de jeu montre un écran de chargement
  aux couleurs du launcher au lieu du noir (avec la progression des shaders), jusqu'à la première scène du
  jeu (`WPGameState`), puis des notifications en haut à droite, une ligne JSON par notification sur stdin
  (format dans `WPOverlay.h`). La fenêtre prend le titre et l'icône du launcher, et les textes de Dolphin
  sa police (M PLUS Rounded 1c, `Data/Sys/Resources/wp`, licence OFL).
- `Source/Core/VideoCommon/OnScreenDisplay.cpp` : avec `WP_OVERLAY`, les messages de Dolphin en haut à
  gauche (« Video Info: … » du GPU, etc.) ne sont plus dessinés : les notifications du launcher les
  remplacent.
- `Source/Core/DiscIO/RiivolutionPatcher.cpp` : avec `WP_GAME_STATE`, un patch mémoire qui ne s'applique
  pas (la mémoire n'est pas ce que le patch attend, valeur introuvable) est signalé sur stdout,
  `wp-patch-skipped <adresse> <dossier>`, le dossier disant au launcher de quel mod il vient.

## Contenu des archives

| OS | Archive | Exécutable |
| --- | --- | --- |
| Linux | `dolphin-wp-linux-x86_64.tar.gz` | `usr/bin/dolphin-emu` |
| Windows | `dolphin-wp-windows-x64.zip` | `Dolphin/Dolphin.exe` |
| macOS | `dolphin-wp-macos-arm64.tar.gz` | `Dolphin.app/Contents/MacOS/Dolphin` |

Le launcher lance toujours Dolphin avec `-u <dossier>` pour garder son propre profil.

## Publier une version

```sh
git tag wp-2609a-1   # <version Dolphin>-<révision Wii Party>
git push origin wp-2609a-1
```

Puis reporter les URL et les sommes de `SHA256SUMS.txt` dans `src-tauri/tools.json` du launcher.

## Construire Linux en local

```sh
git submodule update --init --recursive
cmake -S . -B build/wp-linux -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
  -DENABLE_AUTOUPDATE=OFF -DENABLE_ANALYTICS=OFF -DENABLE_TESTS=OFF -DDISTRIBUTOR=wii-party-launcher
cmake --build build/wp-linux
Tools/wp/package-linux.sh build/wp-linux dist/dolphin-wp-linux-x86_64.tar.gz
```
