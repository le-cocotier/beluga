#!/bin/sh
# An installed beluga (Homebrew, a .deb, winget's zip): its first start
# sets up ~/.beluga from the install's data directory, never overwriting,
# and loads the bundled packages from the install rather than copying them.
# BELUGA_DATA_DIR stands in for <prefix>/share/beluga: the source tree has
# the same config/, packages/ and assets/.
#
# Usage: test/test_installed.sh <beluga binary> <source root>

beluga="$1"
root="$2"
[ -x "$beluga" ] && [ -f "$root/config/init.lisp" ] || { echo "usage: $0 <beluga> <source root>" >&2; exit 2; }
command -v git >/dev/null || { echo "git not found: skipping"; exit 77; }

tmp=$(mktemp -d) || exit 2
trap 'rm -rf "$tmp"' EXIT
failures=0
fail() { echo "FAIL: $*"; failures=$((failures + 1)); }
pass() { echo "ok: $*"; }

run() { HOME="$tmp/home" BELUGA_HOME= BELUGA_DATA_DIR="$root" "$beluga" --sync-packages >"$tmp/out" 2>&1; }

mkdir -p "$tmp/home"
run || { cat "$tmp/out"; fail "first start exits with an error"; }
b="$tmp/home/.beluga"
cmp -s "$root/config/init.lisp" "$b/config/init.lisp" && pass "default config set up" || fail "no config/init.lisp"
cmp -s "$root/config/personal-config.lisp" "$b/packages/personal-config/init.lisp" &&
  pass "personal-config set up" || fail "no personal-config"
[ -f "$b/assets/beluga.txt" ] && pass "splash screen set up" || fail "no assets/beluga.txt"
[ -d "$b/packages/git" ] && fail "bundled packages were copied" || pass "bundled packages not copied"

# The bundled packages load from the install, and the user's own copy of
# one wins over it.
mkdir -p "$b/packages/theme-light"
echo '(define theme-light-was-mine #t)' > "$b/packages/theme-light/init.lisp"
cat > "$b/packages/personal-config/init.lisp" <<LISP
(add-package "theme-light")
(write-data-file "$tmp/probe" (list (package-loaded? "git")
                                    (if (bundled-package-directory "git") #t #f)
                                    (if (and (package-loaded? "theme-light") theme-light-was-mine) #t #f)))
LISP
echo ';; mine' >> "$b/config/init.lisp"
run || { cat "$tmp/out"; fail "second start exits with an error"; }
grep -q ';; mine' "$b/config/init.lisp" && pass "config not overwritten" || fail "config overwritten"
if [ -f "$tmp/probe" ]; then
  [ "$(tr -d '\n ' <"$tmp/probe" | sed 's/^;.*//')" = "#t#t#t" ] &&
    pass "bundled packages load from the install, a user's copy wins" ||
    { cat "$tmp/probe"; fail "probe"; }
else
  cat "$tmp/out"; fail "personal-config didn't run"
fi

exit $((failures > 0))
