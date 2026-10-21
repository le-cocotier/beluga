;; ─── personal-config ────────────────────────────────────────────────────────
;; ~/.beluga/packages/personal-config/init.lisp
;;
;; Your own changes go here. install.sh creates this file once and never
;; touches it again, so everything below survives updates, re-installs and
;; replacing ~/.beluga/config/init.lisp with a newer default — keep that
;; file stock and put your tweaks here instead.
;;
;; Loaded by (add-package "personal-config") at the very end of
;; ~/.beluga/config/init.lisp, after the default packages, theme, key
;; bindings and helper functions. So:
;;   - (define TAB-LENGTH ...) and the other settings override the defaults;
;;   - theme colors set here win over the theme package's;
;;   - a map-key here replaces the default binding for that key and prefix
;;     (the last map-key for a key+prefix is the one that runs);
;;   - every function defined in init.lisp (enter-and-tab,
;;     editor-buffer-picker, ...) exists already, ready to bind.

;; SETTINGS
;; (define TAB-LENGTH 2)
;; (define LINE-NUMBERS #f)
;; (define WHICH-KEY #f)
;; (define LSP #f)

;; LANGUAGE PACKAGES — see the list in ~/.beluga/config/init.lisp
;; (add-package "c")
;; (add-package "python")

;; YOUR PACKAGES — packages that don't ship with beluga. Each line installs
;; the package if it's missing (while beluga starts) and loads it, in this
;; order. The exact commit goes to packages.lock, next to this file: keep
;; both, and `beluga --sync-packages` sets up the same packages elsewhere.
;; CTRL-x c l lists every package: u update, r roll back, d remove, ...
;; (use-package "rainbow" "https://example.org/rainbow.git")           ; default branch
;; (use-package "rainbow" "https://example.org/rainbow.git" "v1.2.0")  ; tag, branch or commit
;; (use-package "rainbow" "https://example.org/rainbow.git" "latest")  ; highest version tag
;; (use-package "mine" "~/code/beluga-mine")                           ; a folder, loaded in place
;;
;; An index lets use-package and package-search find packages by name:
;; (define package-index-url "https://example.org/beluga-packages.git")
;; package-remove moves a package to ~/.beluga/packages.bak; to delete:
;; (define package-remove-deletes #t)

;; THEME OVERRIDES — roles are listed in packages/theme-dark/init.lisp
;; (editor-set-theme-color "line-cursor-background" 50 54 62)

;; KEY BINDINGS
;; (map-key "CTRL-f" editor-switch-next-pane "no-prefix")
;; (map-key "g" lsp-definition "user")

;; HOOKS
;; (add-hook "before-save" (lambda (filename) (lsp-format-buffer)))
