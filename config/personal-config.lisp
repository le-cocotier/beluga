;; ─── personal-config ────────────────────────────────────────────────────────
;; ~/.beluga/packages/personal-config/init.lisp
;;
;; Your own changes go here. install.sh creates this file once and never
;; touches it again, so everything below survives updates, re-installs and
;; replacing ~/.beluga/config/init.lisp with a newer default — keep that
;; file stock and put your tweaks here instead.
;;
;; Loaded by (add-package "personal-config") in ~/.beluga/config/init.lisp:
;; after the default packages and the theme, but BEFORE the default key
;; bindings and helper functions defined further down that file. So:
;;   - (define TAB-LENGTH ...) and the other settings override the defaults;
;;   - theme colors set here win over the theme package's;
;;   - map-key bindings here win over the defaults (the first binding
;;     registered for a key+prefix is the one that runs) — though not over
;;     a package loaded before this one (e.g. outline's CTRL-o);
;;   - but a function defined LATER in init.lisp (enter-and-tab,
;;     editor-buffer-picker, ...) doesn't exist yet here — bind builtins,
;;     your own functions, or a quoted form like '(move-cursor "up").

;; SETTINGS
;; (define TAB-LENGTH 2)
;; (define LINE-NUMBERS #f)
;; (define WHICH-KEY #f)
;; (define LSP #f)

;; LANGUAGE PACKAGES — see the list in ~/.beluga/config/init.lisp
;; (add-package "c")
;; (add-package "python")

;; THEME OVERRIDES — roles are listed in packages/theme-dark/init.lisp
;; (editor-set-theme-color "line-cursor-background" 50 54 62)

;; KEY BINDINGS
;; (map-key "CTRL-f" editor-switch-next-pane "no-prefix")
;; (map-key "g" lsp-definition "user")

;; HOOKS
;; (add-hook "before-save" (lambda (filename) (lsp-format-buffer)))
