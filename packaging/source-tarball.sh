#!/bin/bash
#
# The release's source tarball, what Homebrew and the AUR build from:
#   <outdir>/beluga-<version>.tar.gz, unpacking to beluga-<version>/
#
#   packaging/source-tarball.sh v3.5 [outdir]

set -euo pipefail

VERSION="${1:?usage: $0 VERSION [OUTDIR]}"
VERSION="${VERSION#v}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$(mkdir -p "${2:-$ROOT/dist}" && cd "${2:-$ROOT/dist}" && pwd)"
git -C "$ROOT" archive --format=tar.gz --prefix="beluga-$VERSION/" -o "$OUT/beluga-$VERSION.tar.gz" HEAD
ls -l "$OUT/beluga-$VERSION.tar.gz"
