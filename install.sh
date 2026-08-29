#!/bin/bash
#
# Beluga installer. Run with --help for usage.

set -euo pipefail

# --- output helpers -----------------------------------------------------

if [ -t 1 ]; then
    C_BOLD=$'\033[1m'; C_GREEN=$'\033[32m'; C_YELLOW=$'\033[33m'; C_RED=$'\033[31m'; C_RESET=$'\033[0m'
else
    C_BOLD=''; C_GREEN=''; C_YELLOW=''; C_RED=''; C_RESET=''
fi

info()  { echo "${C_BOLD}==>${C_RESET} $*"; }
ok()    { echo "${C_GREEN}✅${C_RESET} $*"; }
warn()  { echo "${C_YELLOW}⚠️  $*${C_RESET}"; }
error() { echo "${C_RED}❌ $*${C_RESET}" >&2; }
die()   { error "$@"; exit 1; }

# --- option parsing -------------------------------------------------------

ASSUME_YES=0
SKIP_PACKAGES=0
DEBUG_BUILD=0
PREFIX="/usr/local/bin"

usage() {
    cat <<'EOF'
Beluga installer.

Usage: ./install.sh [options]

  -y, --yes            Don't prompt for confirmation; assume "yes" (and
                        never overwrite an existing config).
      --prefix DIR      Where to install the beluga binary (default: /usr/local/bin).
      --skip-packages   Don't clone the default packages listed below.
      --debug           Build an unoptimized debug build with APP_DEBUG logging
                        enabled (stderr debug logs), instead of the default
                        optimized release build.
  -h, --help            Show this help and exit.

Safe to re-run: an existing build/ is reconfigured rather than failing,
already-cloned packages are left alone, and your config is never touched
without asking first.
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        -y|--yes) ASSUME_YES=1 ;;
        --prefix) PREFIX="${2:?--prefix needs a directory}"; shift ;;
        --prefix=*) PREFIX="${1#*=}" ;;
        --skip-packages) SKIP_PACKAGES=1 ;;
        --debug) DEBUG_BUILD=1 ;;
        -h|--help) usage; exit 0 ;;
        *) die "Unknown option: $1 (see --help)" ;;
    esac
    shift
done

confirm() {
    # confirm "question" — returns success (0) for yes.
    [ "$ASSUME_YES" -eq 1 ] && return 0
    local reply
    read -r -p "$1 (y/N) " reply
    [[ "$reply" =~ ^[Yy]$ ]]
}

# --- sanity checks --------------------------------------------------------

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

if [ ! -f "./meson.build" ] || [ ! -f "./assets/beluga.txt" ]; then
    die "This doesn't look like a beluga checkout ($SCRIPT_DIR). Run this script from inside the cloned repo."
fi

echo "${C_BOLD}--- Beluga installer ---${C_RESET}"
confirm "Install beluga from $SCRIPT_DIR?" || { echo "Aborted."; exit 1; }

# --- dependencies ----------------------------------------------------------

detect_pkg_manager() {
    if command -v brew &>/dev/null; then echo "brew"
    elif command -v apt-get &>/dev/null; then echo "apt"
    elif command -v dnf &>/dev/null; then echo "dnf"
    elif command -v pacman &>/dev/null; then echo "pacman"
    else echo "unknown"
    fi
}

install_cmd_for() {
    # install_cmd_for <pkg manager> <package>
    case "$1" in
        brew)   echo "brew install $2" ;;
        apt)    echo "sudo apt-get update && sudo apt-get install -y $2" ;;
        dnf)    echo "sudo dnf install -y $2" ;;
        pacman) echo "sudo pacman -S --noconfirm $2" ;;
    esac
}

check_dep() {
    # check_dep <command> <package name for install commands>
    command -v "$1" &>/dev/null && return 0

    local mgr cmd
    mgr="$(detect_pkg_manager)"
    error "Missing dependency: $1"
    if [ "$mgr" = "unknown" ]; then
        die "Please install '$1' and re-run this script."
    fi
    cmd="$(install_cmd_for "$mgr" "$2")"
    if [ "$ASSUME_YES" -eq 0 ] && confirm "Run \`$cmd\` now to install it?"; then
        eval "$cmd"
        command -v "$1" &>/dev/null || die "'$1' still not found after installing — install it manually and re-run."
    else
        die "Install it yourself, e.g.: $cmd"
    fi
}

info "Checking dependencies ..."
check_dep meson meson
check_dep ninja ninja-build
check_dep git git
if ! command -v cc &>/dev/null && ! command -v gcc &>/dev/null && ! command -v clang &>/dev/null; then
    die "No C compiler found (cc/gcc/clang). Please install one and re-run."
fi
ok "All dependencies present"

# --- config files ----------------------------------------------------------

info "Setting up ~/.beluga ..."
mkdir -pv ~/.beluga/assets/ ~/.beluga/packages/ ~/.beluga/config/ >/dev/null
cp -f ./assets/beluga.txt ~/.beluga/assets/

if [ -f ~/.beluga/config/init.lisp ]; then
    if [ "$ASSUME_YES" -eq 1 ]; then
        cp -f ./config/init.lisp ~/.beluga/config/init.lisp.new
        warn "Kept your existing config; new default written to ~/.beluga/config/init.lisp.new"
    elif confirm "Replace your existing config (old one saved as init.lisp.bak)?"; then
        mv -f ~/.beluga/config/init.lisp ~/.beluga/config/init.lisp.bak
        cp -f ./config/init.lisp ~/.beluga/config/
        ok "Config replaced (backup at ~/.beluga/config/init.lisp.bak)"
    else
        cp -f ./config/init.lisp ~/.beluga/config/init.lisp.new
        ok "Kept your config; new default written to ~/.beluga/config/init.lisp.new"
    fi
else
    cp -f ./config/init.lisp ~/.beluga/config/
    ok "Config installed at ~/.beluga/config/init.lisp"
fi

# --- packages ----------------------------------------------------------

# Packages enabled by default in config/init.lisp (currently just "outline").
# Packages live in their own git repos — same convention as the
# smart_delimiters example ("First git clone it") — so this clones rather
# than copying anything out of this checkout. Already-installed packages are
# left alone (git pull them yourself to update) rather than clobbered, since
# ~/.beluga/packages/ is meant to be user-editable.
#
# One "name url" pair per line — kept as a plain list rather than an
# associative array, since macOS ships bash 3.2 as /bin/bash (no declare -A
# support; that needs bash 4+) and this script's shebang targets /bin/bash.
BELUGA_PACKAGES="
outline https://git.giorgio-nas.fr/beluga/outine.git
smart_delimiters https://git.giorgio-nas.fr/beluga/smart_delimiters.git
c https://git.giorgio-nas.fr/beluga/c.git
explorer https://git.giorgio-nas.fr/beluga/explorer.git
"

if [ "$SKIP_PACKAGES" -eq 1 ]; then
    info "Skipping package installation (--skip-packages)"
else
    info "Installing packages ..."
    echo "$BELUGA_PACKAGES" | while read -r pkg_name pkg_url; do
        [ -z "$pkg_name" ] && continue
        dest=~/.beluga/packages/"$pkg_name"
        if [ -d "$dest" ]; then
            echo "  '$pkg_name' already installed, skipping"
        elif git clone --quiet "$pkg_url" "$dest"; then
            ok "$pkg_name"
        else
            warn "Couldn't clone '$pkg_name' — (add-package \"$pkg_name\") in init.lisp will silently no-op until it's installed"
        fi
    done
fi

# --- build ----------------------------------------------------------

if [ "$DEBUG_BUILD" -eq 1 ]; then
    info "Building (debug, APP_DEBUG logging enabled) ..."
    MESON_ARGS=(-Dbuildtype=debug -Dc_args=-DAPP_DEBUG)
else
    info "Building (release) ..."
    MESON_ARGS=(-Dbuildtype=release)
fi

if [ -d build ]; then
    meson setup --reconfigure build "${MESON_ARGS[@]}"
else
    meson setup build "${MESON_ARGS[@]}"
fi
meson compile -C build

# --- install the binary ----------------------------------------------------------

info "Installing to $PREFIX ..."
if [ ! -d "$PREFIX" ]; then
    mkdir -p "$PREFIX" 2>/dev/null || sudo mkdir -p "$PREFIX"
fi
if [ -w "$PREFIX" ]; then
    cp -f ./build/beluga "$PREFIX/"
else
    warn "$PREFIX isn't writable by you — using sudo"
    sudo cp -f ./build/beluga "$PREFIX/"
fi

echo
ok "Installed $("$PREFIX/beluga" --version) to $PREFIX/beluga"
if ! command -v beluga &>/dev/null; then
    warn "$PREFIX isn't on your PATH yet — add it, e.g.: export PATH=\"$PREFIX:\$PATH\""
fi
echo "Check ~/.beluga/config/init.lisp for customization."
