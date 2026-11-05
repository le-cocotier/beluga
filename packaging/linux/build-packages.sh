#!/bin/bash
#
# Builds beluga_<version>_amd64.deb and beluga-<version>-1.x86_64.rpm from
# this checkout, into dist/ (or the folder given as the second argument).
#
#   packaging/linux/build-packages.sh 3.5 [outdir]
#
# Both install /usr/bin/beluga and /usr/share/beluga (default config, the
# bundled packages, the splash screen); beluga sets up ~/.beluga from those
# on its first start. Needs meson, ninja, a C compiler, dpkg-deb and
# rpmbuild (apt-get install meson ninja-build gcc rpm). Run by
# .gitea/workflows/release-linux.yml on every v* tag.

set -euo pipefail

VERSION="${1:?usage: $0 VERSION [OUTDIR]}"
VERSION="${VERSION#v}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$(mkdir -p "${2:-$ROOT/dist}" && cd "${2:-$ROOT/dist}" && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

ARCH_DEB="$(dpkg --print-architecture 2>/dev/null || echo amd64)"
ARCH_RPM="$(uname -m)"
SUMMARY="Terminal text editor configured in Lisp, with LSP and a debugger"
HOMEPAGE="https://git.giorgio-nas.fr/arthur/beluga"
MAINTAINER="Arthur Barraux <arthurbarraux@icloud.com>"
LICENSE="MIT"

# --- build and stage -------------------------------------------------------
meson setup "$WORK/build" "$ROOT" --prefix=/usr --buildtype=release >/dev/null
meson compile -C "$WORK/build"
DESTDIR="$WORK/root" meson install -C "$WORK/build" --no-rebuild >/dev/null
strip "$WORK/root/usr/bin/beluga"
mkdir -p "$WORK/root/usr/share/doc/beluga"
cp "$ROOT/README.md" "$WORK/root/usr/share/doc/beluga/"
[ -f "$ROOT/LICENSE" ] && cp "$ROOT/LICENSE" "$WORK/root/usr/share/doc/beluga/copyright"

# --- .deb ------------------------------------------------------------------
DEB="$WORK/deb"
cp -a "$WORK/root" "$DEB"
mkdir -p "$DEB/DEBIAN"
# The C library and libutil are all it links; dpkg-shlibdeps would say the
# same, but needs a debian/ tree to run.
cat > "$DEB/DEBIAN/control" <<CONTROL
Package: beluga
Version: $VERSION
Architecture: $ARCH_DEB
Maintainer: $MAINTAINER
Installed-Size: $(du -sk "$WORK/root" | cut -f1)
Depends: libc6
Recommends: git
Suggests: gdb, clangd
Section: editors
Priority: optional
Homepage: $HOMEPAGE
Description: $SUMMARY
 Beluga is a terminal text editor for Linux, macOS and Windows, written in
 C and configured in Lisp. It has language servers (LSP), a debugger (DAP),
 a git client, a package manager, terminal panes and a daemon mode.
CONTROL
dpkg-deb --root-owner-group --build "$DEB" "$OUT/beluga_${VERSION}_${ARCH_DEB}.deb"

# --- .rpm ------------------------------------------------------------------
RPMTOP="$WORK/rpm"
mkdir -p "$RPMTOP"/{SPECS,BUILD,RPMS,SRPMS,SOURCES}
cat > "$RPMTOP/SPECS/beluga.spec" <<SPEC
Name:           beluga
Version:        $VERSION
Release:        1
Summary:        $SUMMARY
License:        $LICENSE
URL:            $HOMEPAGE
Recommends:     git
%global debug_package %{nil}
%global _build_id_links none
%global __os_install_post %{nil}

%description
Beluga is a terminal text editor for Linux, macOS and Windows, written in
C and configured in Lisp. It has language servers (LSP), a debugger (DAP),
a git client, a package manager, terminal panes and a daemon mode.

%install
cp -a "$WORK/root/." "%{buildroot}/"

%files
/usr/bin/beluga
/usr/share/beluga
%doc /usr/share/doc/beluga
SPEC
rpmbuild --quiet -bb --define "_topdir $RPMTOP" "$RPMTOP/SPECS/beluga.spec"
cp "$RPMTOP"/RPMS/*/beluga-*.rpm "$OUT/"

ls -l "$OUT"
