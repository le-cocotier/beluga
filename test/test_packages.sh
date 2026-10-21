#!/bin/sh
# The package manager, end to end: beluga --sync-packages against a
# throwaway BELUGA_HOME and package repositories made on the spot (file://
# URLs, so no network). Checks installing, dependencies, refs, the
# lockfile, the index, update/rollback/remove and add-package's refusals.
#
# Usage: test/test_packages.sh <beluga binary> <source root>

beluga="$1"
root="$2"
[ -x "$beluga" ] && [ -d "$root/packages/package" ] || { echo "usage: $0 <beluga> <source root>" >&2; exit 2; }
command -v git >/dev/null || { echo "git not found: skipping"; exit 77; }

tmp=$(mktemp -d) || exit 2
trap 'rm -rf "$tmp"' EXIT
home="$tmp/home"
remotes="$tmp/remotes"
failures=0

export GIT_CONFIG_GLOBAL="$tmp/gitconfig"
git config --global user.email test@example.org
git config --global user.name test
git config --global init.defaultBranch main
git config --global protocol.file.allow always

fail() { echo "FAIL: $*"; failures=$((failures + 1)); }
pass() { echo "ok: $*"; }

# make_repo <name> — a repository in $tmp/work/<name>, published bare to
# $remotes/<name>.git by publish.
make_repo() {
    mkdir -p "$tmp/work" "$remotes"
    git init -q "$tmp/work/$1"
}
commit() { # commit <name> <message> [tag]
    git -C "$tmp/work/$1" add -A
    git -C "$tmp/work/$1" commit -qm "$2"
    [ -n "$3" ] && git -C "$tmp/work/$1" tag "$3"
    if [ -d "$remotes/$1.git" ]; then
        git -C "$tmp/work/$1" push -q --tags "$remotes/$1.git" HEAD:main
    else
        git clone -q --bare "$tmp/work/$1" "$remotes/$1.git"
    fi
}
url() { echo "file://$remotes/$1.git"; }

# alpha: v1.0.0, v1.1.0, then an untagged commit on main.
make_repo alpha
echo '(define-package "alpha" (version "1.0.0") (description "first"))' > "$tmp/work/alpha/package.lisp"
echo '(define alpha-value 1)' > "$tmp/work/alpha/init.lisp"
commit alpha one v1.0.0
echo '(define-package "alpha" (version "1.1.0"))' > "$tmp/work/alpha/package.lisp"
echo '(define alpha-value 2)' > "$tmp/work/alpha/init.lisp"
commit alpha two v1.1.0
echo '(define alpha-value 3)' > "$tmp/work/alpha/init.lisp"
commit alpha three

# beta requires alpha >= 1.0 and says where to get it.
make_repo beta
cat > "$tmp/work/beta/package.lisp" <<EOF
(define-package "beta"
  (version "0.1")
  (requires ("beluga" "1.0") ("alpha" "1.0"))
  (sources ("alpha" "$(url alpha)")))
EOF
echo '(define beta-saw alpha-value)' > "$tmp/work/beta/init.lisp"
commit beta one

# config: the package manager, then personal-config, like the default one.
reset_home() {
    rm -rf "$home"
    mkdir -p "$home/config" "$home/packages/personal-config"
    cp -R "$root/packages/package" "$home/packages/package"
    cat > "$home/config/init.lisp" <<'EOF'
(define TAB-LENGTH 2)
(define QUIT-TIMES 1)
(define LSP #f)
(add-package "package")
(add-package "personal-config")
EOF
}
personal() { cat > "$home/packages/personal-config/init.lisp"; }
run() {
    BELUGA_HOME="$home" "$beluga" --sync-packages </dev/null >"$tmp/out" 2>&1
    echo $? >"$tmp/status"
    cat "$tmp/out" >>"$tmp/all"
}
status() { cat "$tmp/status"; }
out_has() { grep -qF -- "$1" "$tmp/out"; }
lock_has() { grep -qF -- "$1" "$home/packages/personal-config/packages.lock" 2>/dev/null; }
head_of() { git -C "$home/packages/$1" rev-parse HEAD; }
commit_of() { git -C "$remotes/$1.git" rev-parse "$2^{commit}"; }

# 1. A fresh install: beta, and alpha as its dependency, loaded first.
reset_home
personal <<EOF
(use-package "beta" "$(url beta)")
(editor-message (string-append "beta-saw=" (number->string beta-saw)))
EOF
run
if [ "$(status)" = 0 ] && out_has "beta-saw=3" && lock_has '(package "alpha"' && lock_has '(via "beta")' &&
   [ "$(head_of alpha)" = "$(commit_of alpha main)" ]; then
    pass "install with a dependency"
else
    fail "install with a dependency"; cat "$tmp/out"
fi

# 2. Nothing to do the second time.
run
if [ "$(status)" = 0 ] && out_has "Packages in sync" && ! out_has "Installing"; then
    pass "second sync does nothing"
else
    fail "second sync does nothing"; cat "$tmp/out"
fi

# 3. A tag, and "latest".
reset_home
personal <<EOF
(use-package "alpha" "$(url alpha)" "v1.0.0")
(editor-message (string-append "alpha=" (number->string alpha-value)))
EOF
run
if [ "$(status)" = 0 ] && out_has "alpha=1" && lock_has '(ref "v1.0.0")'; then
    pass "pinned to a tag"
else
    fail "pinned to a tag"; cat "$tmp/out"
fi
# Changing the tag in the config switches at the next start, without a fetch.
sed -i.bak 's/"v1.0.0"/"v1.1.0"/' "$home/packages/personal-config/init.lisp"
run
if [ "$(status)" = 0 ] && out_has "alpha=2" && out_has "alpha: now at v1.1.0" && lock_has '(ref "v1.1.0")' &&
   lock_has "(previous \"$(commit_of alpha v1.0.0)\")"; then
    pass "a new tag in the config"
else
    fail "a new tag in the config"; cat "$tmp/out"
fi
# A branch the clone doesn't have yet: package-sync fetches it.
git -C "$tmp/work/alpha" push -q "$remotes/alpha.git" HEAD:stable
sed -i.bak 's/"v1.1.0"/"stable"/' "$home/packages/personal-config/init.lisp"
run
if [ "$(status)" = 0 ] && out_has "run package-sync to fetch it" && out_has "alpha: now at stable" &&
   [ "$(head_of alpha)" = "$(commit_of alpha stable)" ]; then
    pass "a branch fetched by package-sync"
else
    fail "a branch fetched by package-sync"; cat "$tmp/out"
fi
reset_home
personal <<EOF
(use-package "alpha" "$(url alpha)" "latest")
(editor-message (string-append "alpha=" (number->string alpha-value)))
EOF
run
if [ "$(status)" = 0 ] && out_has "alpha=2"; then pass "latest tag"; else fail "latest tag"; cat "$tmp/out"; fi

# 4. The lock wins over the ref: a new machine gets the locked commit.
lock="$home/packages/personal-config/packages.lock"
cp "$lock" "$tmp/lock"
cp "$home/packages/personal-config/init.lisp" "$tmp/personal"
reset_home
cp "$tmp/personal" "$home/packages/personal-config/init.lisp"
sed "s/$(commit_of alpha v1.1.0)/$(commit_of alpha v1.0.0)/" "$tmp/lock" > "$lock"
run
if [ "$(status)" = 0 ] && out_has "alpha=1" && [ "$(head_of alpha)" = "$(commit_of alpha v1.0.0)" ]; then
    pass "install at the locked commit"
else
    fail "install at the locked commit"; cat "$tmp/out"
fi

# 5. sync moves an installed package to the locked commit.
git -C "$home/packages/alpha" checkout -q --detach "$(commit_of alpha main)"
run
if [ "$(status)" = 0 ] && [ "$(head_of alpha)" = "$(commit_of alpha v1.0.0)" ]; then
    pass "sync to the locked commit"
else
    fail "sync to the locked commit"; cat "$tmp/out"
fi

# 6. Update, then roll back (in the foreground nothing is asked).
reset_home
personal <<EOF
(use-package "alpha" "$(url alpha)" "v1.0.0")
EOF
run
cat >> "$home/packages/personal-config/init.lisp" <<'EOF'
(package--declare! "alpha" #f "latest")
(package-update "alpha")
EOF
run
if out_has "Updated alpha" && [ "$(head_of alpha)" = "$(commit_of alpha v1.1.0)" ] &&
   lock_has "(previous \"$(commit_of alpha v1.0.0)\")"; then
    pass "update"
else
    fail "update"; cat "$tmp/out"
fi
personal <<EOF
(use-package "alpha" "$(url alpha)" "latest")
(package-rollback "alpha")
EOF
run
if out_has "Rolled alpha back" && [ "$(head_of alpha)" = "$(commit_of alpha v1.0.0)" ]; then
    pass "rollback"
else
    fail "rollback"; cat "$tmp/out"
fi

# 7. Local changes are never overwritten.
echo ';; mine' >> "$home/packages/alpha/init.lisp"
personal <<EOF
(use-package "alpha" "$(url alpha)" "latest")
(package-update "alpha")
EOF
run
if out_has "has local changes" && [ "$(head_of alpha)" = "$(commit_of alpha v1.0.0)" ]; then
    pass "local changes kept"
else
    fail "local changes kept"; cat "$tmp/out"
fi

# 8. remove moves the package to packages.bak and drops it from the lock.
personal <<'EOF'
(package-remove "alpha")
EOF
run
if out_has "Removed alpha" && [ ! -d "$home/packages/alpha" ] && ls "$home/packages.bak" | grep -q '^alpha-' &&
   ! lock_has '(package "alpha"'; then
    pass "remove"
else
    fail "remove"; cat "$tmp/out"
fi

# 9. add-package's refusals: too old a beluga, a missing dependency, a cycle.
reset_home
for p in future needy ying yang; do mkdir -p "$home/packages/$p"; done
echo '(define-package "future" (requires ("beluga" "999")))' > "$home/packages/future/package.lisp"
echo '(define future-loaded #t)' > "$home/packages/future/init.lisp"
echo '(define-package "needy" (requires ("nowhere" "1.0")))' > "$home/packages/needy/package.lisp"
echo '(define needy-loaded #t)' > "$home/packages/needy/init.lisp"
echo '(define-package "ying" (requires ("yang")))' > "$home/packages/ying/package.lisp"
echo '(define ying-loaded #t)' > "$home/packages/ying/init.lisp"
echo '(define-package "yang" (requires ("ying")))' > "$home/packages/yang/package.lisp"
echo '(define yang-loaded #t)' > "$home/packages/yang/init.lisp"
personal <<'EOF'
(editor-message (if (add-package "future") "future loaded" "future refused"))
(editor-message (if (add-package "needy") "needy loaded" "needy refused"))
(editor-message (if (add-package "ying") "ying loaded" "ying refused"))
(editor-message (string-append "yang " (package-state "yang")))
EOF
run
if out_has "needs beluga 999" && out_has "future refused" && out_has "needy refused" &&
   out_has "require each other" && out_has "ying loaded" && out_has "yang loaded"; then
    pass "add-package refusals"
else
    fail "add-package refusals"; cat "$tmp/out"
fi

# 10. A local folder, loaded in place; a missing package with no URL.
reset_home
mkdir -p "$tmp/mine"
echo '(define mine-loaded "yes")' > "$tmp/mine/init.lisp"
personal <<EOF
(use-package "mine" "$tmp/mine")
(editor-message (string-append "mine=" mine-loaded))
(use-package "ghost")
EOF
run
if out_has "mine=yes" && out_has '"ghost" isn'"'"'t installed and has no URL' && [ "$(status)" = 1 ] &&
   [ ! -e "$home/packages/mine" ]; then
    pass "local folder, and a package with no URL"
else
    fail "local folder, and a package with no URL"; cat "$tmp/out"
fi

# 11. The index: a package found by name.
make_repo index
echo "(package \"beta\" (url \"$(url beta)\") (description \"the second\"))" > "$tmp/work/index/index.lisp"
commit index one
reset_home
personal <<EOF
(define package-index-url "$(url index)")
(use-package "beta")
(editor-message (string-append "beta-saw=" (number->string beta-saw)))
EOF
run
if [ "$(status)" = 0 ] && out_has "beta-saw=3" && [ -f "$home/index/index.lisp" ]; then
    pass "install from the index"
else
    fail "install from the index"; cat "$tmp/out"
fi

# 12. Packages nothing declares any more are reported.
personal <<'EOF'
(use-package "beta")
EOF
git clone -q "$(url alpha)" "$home/packages/stray"
run
if out_has "Not declared: stray"; then pass "undeclared packages reported"; else fail "undeclared packages reported"; cat "$tmp/out"; fi

# A sanitizer build reports here even where a check above ignored the status.
if grep -E "ERROR: AddressSanitizer|ERROR: LeakSanitizer|runtime error:" "$tmp/all"; then
    fail "sanitizer errors (above)"
fi

[ "$failures" -eq 0 ] || { echo "$failures failed"; exit 1; }
echo "all passed"
