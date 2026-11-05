#!/bin/bash
#
# Cross-compiles beluga.exe on Linux and assembles the Windows release zip,
#   <outdir>/beluga-<tag>-windows-x64.zip
# with beluga.exe, install.ps1, LICENSE, README.txt, the default config,
# the bundled packages and the splash screen. Windows users unzip it and
# run install.ps1; winget installs it as a portable app.
#
#   packaging/windows/build-zip.sh v3.5 [outdir]
#
# Needs meson, ninja, mingw-w64 and zip. Used by the release workflows in
# .gitea/workflows and .github/workflows.

set -euo pipefail

TAG="${1:?usage: $0 TAG [OUTDIR]}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$(mkdir -p "${2:-$ROOT/dist}" && cd "${2:-$ROOT/dist}" && pwd)"
PKG="beluga-$TAG-windows-x64"
BUILD="$ROOT/build-windows-release"

cd "$ROOT"
meson setup --cross-file cross/mingw-w64.ini --buildtype=release --reconfigure "$BUILD" >/dev/null 2>&1 ||
  meson setup --cross-file cross/mingw-w64.ini --buildtype=release "$BUILD" >/dev/null
meson compile -C "$BUILD" beluga
# Debug info is useless to users and doubles the size.
x86_64-w64-mingw32-strip "$BUILD/beluga.exe"

rm -rf "${OUT:?}/$PKG" "$OUT/$PKG.zip"
mkdir -p "$OUT/$PKG"
cp "$BUILD/beluga.exe" install.ps1 LICENSE "$OUT/$PKG/"
cp windows/README.txt "$OUT/$PKG/README.txt"
cp -r assets config packages "$OUT/$PKG/"
# The editor writes this next to wherever it runs; never ship one.
find "$OUT/$PKG" -name lisp_log.err -delete
(cd "$OUT" && zip -qr9 "$PKG.zip" "$PKG")
rm -rf "${OUT:?}/$PKG"
ls -l "$OUT/$PKG.zip"
