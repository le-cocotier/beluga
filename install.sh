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
DEV_MODE=0
DEBUG_BUILD=0
UPDATE=0
PREFIX="/usr/local/bin"

usage() {
    cat <<'EOF'
Beluga installer.

Usage: ./install.sh [options]

  -y, --yes            Don't prompt for confirmation; assume "yes" (and
                        never overwrite an existing config).
      --prefix DIR      Where to install the beluga binary (default: /usr/local/bin).
      --skip-packages   Don't install the bundled packages (packages/ in
                        this checkout).
      --dev             Symlink the bundled packages to this checkout instead
                        of copying them, for working on them: edits there are
                        live in the editor and tracked by this repo's git.
      --update          Pull the new version.
      --debug           Build an unoptimized debug build with APP_DEBUG logging
                        enabled, instead of the default optimized release
                        build. Logs (the editor's own trace calls, plus the
                        language server's own stderr chatter) go to
                        ~/.beluga/debug.log, not the terminal — `tail -f`
                        it in another pane while you work.
  -h, --help            Show this help and exit.

Safe to re-run: an existing build/ is reconfigured rather than failing,
and your config is never touched without asking first. Bundled packages
are replaced to match this checkout; one with local edits (or an old git
clone of it) is moved to ~/.beluga/packages.bak/ first, never deleted.
Packages that aren't bundled are never touched.

Your own changes belong in ~/.beluga/packages/personal-config/init.lisp:
it's created from a template on the first install and never overwritten
afterwards (even with --yes or --skip-packages), so it survives updates.
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        -y|--yes) ASSUME_YES=1 ;;
        --prefix) PREFIX="${2:?--prefix needs a directory}"; shift ;;
        --prefix=*) PREFIX="${1#*=}" ;;
        --skip-packages) SKIP_PACKAGES=1 ;;
        --dev) DEV_MODE=1 ;;
        --update) UPDATE=1 ;;
        --debug) DEBUG_BUILD=1 ;;
        -h|--help) usage; exit 0 ;;
        *) die "Unknown option: $1 (see --help)" ;;
    esac
    shift
done

update() {
    # update the repo
    git pull
}

if [ "$UPDATE" -eq 1 ]; then
  update
fi

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
    # -y means "assume yes to every prompt", which has to include this one —
    # it used to mean the opposite here: [ "$ASSUME_YES" -eq 0 ] && confirm
    # is false the moment ASSUME_YES is 1, so --yes skipped straight past
    # the install and died on "still missing" instead of just installing it,
    # defeating the entire point of an unattended/-y run on a fresh machine.
    if [ "$ASSUME_YES" -eq 1 ] || confirm "Run \`$cmd\` now to install it?"; then
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

# personal-config: the one package that's yours rather than a git clone —
# created once from the template, then never overwritten (not even with
# --yes or on re-install), so edits there survive every update. Independent
# of --skip-packages for the same reason.
PERSONAL_CONFIG=~/.beluga/packages/personal-config/init.lisp
if [ -f "$PERSONAL_CONFIG" ]; then
    echo "  personal-config already exists, leaving it alone"
else
    mkdir -p "$(dirname "$PERSONAL_CONFIG")"
    cp ./config/personal-config.lisp "$PERSONAL_CONFIG"
    ok "Personal config created at $PERSONAL_CONFIG"
fi
# A config from before personal-config existed never loads it, so edits
# there would silently do nothing.
if ! grep -q '^[[:space:]]*(add-package[[:space:]]*"personal-config")' ~/.beluga/config/init.lisp; then
    warn "~/.beluga/config/init.lisp doesn't load personal-config — add (add-package \"personal-config\") after the theme's add-package line"
fi

# --- packages ----------------------------------------------------------

# Bundled packages ship in this checkout's packages/ directory, versioned
# with the editor itself: they call builtins that only exist from a given
# version on (dired's process-run, outline's editor-recenter), so they're
# installed from the same checkout as the binary instead of being cloned
# from repos of their own that could drift out of step with it. A normal
# install copies them; --dev symlinks each one to the checkout instead, so
# editing a package there is live in the editor and shows up in this
# repo's git status right next to the C changes.
#
# ~/.beluga/packages/<name> for a bundled package is beluga's to replace on
# every install — that's what keeps it in step with the binary. Anything
# that would be lost doing so (a copy with local edits, or an old git
# clone from before packages were bundled) is moved to
# ~/.beluga/packages.bak/<name>-<timestamp> first, never deleted. Packages
# that aren't bundled — personal-config, your own, third-party ones — are
# never touched.

PACKAGES_SRC="$SCRIPT_DIR/packages"
PACKAGES_BAK=~/.beluga/packages.bak
STAMP="$(date +%Y%m%d-%H%M%S)"

backup_package() {
    # backup_package <installed dir> <name> <why>
    mkdir -p "$PACKAGES_BAK"
    mv "$1" "$PACKAGES_BAK/$2-$STAMP"
    warn "'$2' $3 — moved to $PACKAGES_BAK/$2-$STAMP"
}

install_package() {
    # install_package <name> — prints nothing when already up to date
    local name="$1"
    local src="$PACKAGES_SRC/$1"
    local dest=~/.beluga/packages/"$1"

    if [ -L "$dest" ]; then
        if [ "$DEV_MODE" -eq 1 ] && [ "$(readlink "$dest")" = "$src" ]; then
            return 0
        fi
        rm "$dest"   # a symlink holds nothing of its own to lose
    elif [ -d "$dest/.git" ]; then
        backup_package "$dest" "$name" "was a git clone (now bundled)"
    elif [ -d "$dest" ]; then
        if diff -rq -x lisp_log.err -x .DS_Store "$src" "$dest" >/dev/null 2>&1; then
            if [ "$DEV_MODE" -eq 0 ]; then
                return 0
            fi
            rm -rf "$dest"   # identical to the bundled copy: nothing to keep
        else
            backup_package "$dest" "$name" "had local changes"
        fi
    elif [ -e "$dest" ]; then
        backup_package "$dest" "$name" "wasn't a directory"
    fi

    if [ "$DEV_MODE" -eq 1 ]; then
        ln -s "$src" "$dest"
        echo "  $name -> $src"
    else
        cp -R "$src" "$dest"
        echo "  $name"
    fi
}

if [ "$SKIP_PACKAGES" -eq 1 ]; then
    info "Skipping bundled packages (--skip-packages)"
elif [ ! -d "$PACKAGES_SRC" ]; then
    warn "No packages/ directory in this checkout — skipping bundled packages"
else
    if [ "$DEV_MODE" -eq 1 ]; then
        info "Linking bundled packages to $PACKAGES_SRC (--dev) ..."
    else
        info "Installing bundled packages ..."
    fi
    for pkg_dir in "$PACKAGES_SRC"/*/; do
        install_package "$(basename "$pkg_dir")"
    done
    ok "Bundled packages up to date"
fi

# --- build ----------------------------------------------------------

if [ "$DEBUG_BUILD" -eq 1 ]; then
    info "Building (debug, APP_DEBUG logging enabled) ..."
    MESON_ARGS=(-Dbuildtype=debug -Dc_args=-DAPP_DEBUG)
else
    info "Building (release) ..."
    # -Dc_args= explicitly: `meson setup --reconfigure` keeps every option
    # it isn't given, so after one --debug install the -DAPP_DEBUG from it
    # silently stayed in every "release" build that followed.
    MESON_ARGS=(-Dbuildtype=release -Dc_args=)
fi

if [ -d build ]; then
    meson setup --reconfigure build "${MESON_ARGS[@]}"
else
    meson setup build "${MESON_ARGS[@]}"
fi
if ! meson compile -C build; then
    die "Build failed"
fi

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
echo "Put your customizations in ~/.beluga/packages/personal-config/init.lisp — unlike"
echo "~/.beluga/config/init.lisp, it's never replaced by an update."
