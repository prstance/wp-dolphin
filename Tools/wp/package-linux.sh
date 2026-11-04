#!/usr/bin/env bash
# Packs a CMake build of Dolphin into a self-contained folder for the Wii Party
# launcher, then tars it. Layout inside the archive:
#   usr/bin/dolphin-emu, usr/bin/dolphin-tool  executables (RPATH -> usr/lib)
#   usr/bin/Sys/                               Dolphin reads Sys next to its executable first
#   usr/lib/, usr/plugins/                     bundled libraries and Qt plugins
#
# usage: Tools/wp/package-linux.sh <cmake build dir> <output.tar.gz>
# env:   QMAKE  path to Qt 6 qmake (default: qmake6, else qmake)
set -euo pipefail

BUILD="$(realpath "$1")"
OUT="$(realpath -m "$2")"
TOOLS="${BUILD}/wp-tools"

LINUXDEPLOY_URL="https://github.com/linuxdeploy/linuxdeploy/releases/download/1-alpha-20251107-1/linuxdeploy-x86_64.AppImage"
LINUXDEPLOY_SHA256="c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d"
PLUGIN_QT_URL="https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/1-alpha-20250213-1/linuxdeploy-plugin-qt-x86_64.AppImage"
PLUGIN_QT_SHA256="15106be885c1c48a021198e7e1e9a48ce9d02a86dd0a1848f00bdbf3c1c92724"

fetch() { # url sha256 dest
  if [[ ! -x "$3" ]] || ! echo "$2  $3" | sha256sum -c --status; then
    curl -fsSL -o "$3" "$1"
    echo "$2  $3" | sha256sum -c --quiet
    chmod +x "$3"
  fi
}
mkdir -p "${TOOLS}"
fetch "${LINUXDEPLOY_URL}" "${LINUXDEPLOY_SHA256}" "${TOOLS}/linuxdeploy-x86_64.AppImage"
fetch "${PLUGIN_QT_URL}" "${PLUGIN_QT_SHA256}" "${TOOLS}/linuxdeploy-plugin-qt-x86_64.AppImage"

APPDIR="${BUILD}/wp-appdir"
rm -rf "${APPDIR}"
DESTDIR="${APPDIR}" cmake --install "${BUILD}"
mv "${APPDIR}/usr/share/dolphin-emu/Sys" "${APPDIR}/usr/bin/Sys"

export QMAKE="${QMAKE:-$(command -v qmake6 || command -v qmake)}"
export APPIMAGE_EXTRACT_AND_RUN=1 # no FUSE on CI runners
export NO_STRIP=1               # its bundled strip chokes on newer libraries (.relr.dyn)
export PATH="${TOOLS}:${PATH}"
linuxdeploy-x86_64.AppImage --appdir "${APPDIR}" \
  --executable "${APPDIR}/usr/bin/dolphin-emu" \
  --executable "${APPDIR}/usr/bin/dolphin-tool" \
  --desktop-file "${APPDIR}/usr/share/applications/dolphin-emu.desktop" \
  --icon-file "${APPDIR}/usr/share/icons/hicolor/256x256/apps/dolphin-emu.png" \
  --plugin qt

mkdir -p "$(dirname "${OUT}")"
tar -C "${APPDIR}" -czf "${OUT}" .
echo "==> ${OUT}"
