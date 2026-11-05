#!/bin/bash
#
# Fills in the package-manager recipes for a published release.
#
#   packaging/render.sh 3.5
#
# Downloads that release's source tarball and Windows zip from Gitea,
# hashes them, and writes ready-to-commit copies of every recipe to
# dist/packaging/<version>/, laid out like the repositories they go to:
#
#   homebrew-beluga/Formula/beluga.rb       -> git.giorgio-nas.fr/arthur/homebrew-beluga
#   winget-pkgs/manifests/a/ArthurBarraux/Beluga/<version>/*.yaml
#                                           -> a PR to github.com/microsoft/winget-pkgs
#   aur/beluga-editor/PKGBUILD (+.SRCINFO)  -> ssh://aur@aur.archlinux.org/beluga-editor.git
#
# The templates next to this script keep their @VERSION@ placeholders.

set -euo pipefail

VERSION="${1:?usage: $0 VERSION}"
VERSION="${VERSION#v}"
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$HERE/../dist/packaging/$VERSION"
BASE="${BELUGA_RELEASE_URL:-https://git.giorgio-nas.fr/arthur/beluga/releases/download}/v$VERSION"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

sha() {  # sha256 of a downloaded release file
    curl -fsSL -o "$TMP/$1" "$BASE/$1" || { echo "Can't download $BASE/$1 — is v$VERSION released?" >&2; exit 1; }
    if command -v sha256sum >/dev/null; then sha256sum "$TMP/$1" | cut -d' ' -f1
    else shasum -a 256 "$TMP/$1" | cut -d' ' -f1; fi
}

SHA_SOURCE="$(sha "beluga-$VERSION.tar.gz")"
SHA_WINDOWS="$(sha "beluga-v$VERSION-windows-x64.zip")"
DATE="$(date +%Y-%m-%d)"

render() {  # template -> destination
    mkdir -p "$(dirname "$2")"
    sed -e "s|@VERSION@|$VERSION|g" -e "s|@SHA256_SOURCE@|$SHA_SOURCE|g" \
        -e "s|@SHA256_WINDOWS@|$SHA_WINDOWS|g" -e "s|@DATE@|$DATE|g" "$1" > "$2"
}

rm -rf "$OUT"
render "$HERE/homebrew/beluga.rb" "$OUT/homebrew-beluga/Formula/beluga.rb"
WINGET="$OUT/winget-pkgs/manifests/a/ArthurBarraux/Beluga/$VERSION"
for f in "$HERE"/winget/*.yaml; do
    render "$f" "$WINGET/$(basename "$f")"
done
render "$HERE/arch/PKGBUILD" "$OUT/aur/beluga-editor/PKGBUILD"
if command -v makepkg >/dev/null; then
    (cd "$OUT/aur/beluga-editor" && makepkg --printsrcinfo > .SRCINFO)
else
    echo "makepkg isn't installed: generate aur/beluga-editor/.SRCINFO on Arch (makepkg --printsrcinfo > .SRCINFO)"
fi

echo "source  $SHA_SOURCE"
echo "windows $SHA_WINDOWS"
find "$OUT" -type f | sed "s|^$OUT/|  |"
