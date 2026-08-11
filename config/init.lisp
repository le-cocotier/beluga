;; MACROS

(define TAB-LENGTH 4)
(define QUIT-TIMES 1)
(define LSP #t)

;; Prefixes need to exist before anything below tries to (map-key ... "user")
;; or "term" — including packages, which load next. A package that tried to
;; bind under a not-yet-registered prefix would silently fall back to
;; "no-prefix" instead (map-key resolves the prefix name to an id right
;; away, at load time, not when the key is actually pressed).
(add-prefix "user")
(add-prefix "term")

;; PACKAGES

;; First git clone it
;; (add-package "smart_delimiters")
(add-package "outline")
(add-package "explorer")

;; Themes: exactly one should be loaded (the second one just overwrites
;; whatever colors the first one set). Fork either as a starting point
;; for your own — see packages/theme-dark/init.lisp for the interface.
(add-package "theme-dark")
;; (add-package "theme-light")

;; Per-language packages: each registers its own
;; (lsp-server "ext" "language-id" "command" "arg1" ...) — which spawns
;; that language server the first time a matching file is opened — plus
;; a CTRL-d comment-toggle keybind. See packages/<name>/init.lisp. Same
;; "git clone it first" convention as smart_delimiters above. Calling
;; (lsp-server ...) directly here also works fine for a language with no
;; package of its own yet.
(add-package "c")
;; (add-package "python")   ; needs pyright-langserver on PATH
;; (add-package "rust")     ; needs rust-analyzer on PATH

;; HOOKS
;;
;; (add-hook "event-name" fn) runs fn whenever that event fires. fn must be
;; a lambda or a symbol bound to one (not the '(quoted-list) form map-key
;; uses). Currently fired events:
;;   "buffer-opened" (filename buffer-id) - a file finished loading
;;   "before-save"   (filename)           - about to write buffer to disk
;;   "after-save"    (filename)           - buffer was just written
;;   "lsp-ready"     ()                   - LSP handshake completed
;;
;; Example: log every save to the status bar.
;; (add-hook "after-save" (lambda (filename) (editor-set-prefix "no-prefix")))


;; INTROSPECTION
;;
;; (line-text [row]) returns the text of the given (or current) line as a
;; string, (cursor-row) / (cursor-col) return the cursor's position,
;; (buffer-line-count) the number of lines, (buffer-filename) the current
;; buffer's absolute path. These let Lisp functions make real decisions
;; instead of only moving the cursor blindly.

;; FUNCTIONS

;; Body wrapped in (begin ...) so both forms actually run in sequence —
;; without it, "(move-cursor "right")" (evaluating to a boolean) got
;; treated as an operator applied to "(editor-delete-previous-char)"'s
;; result instead of two statements. That silently worked when this was
;; only ever invoked directly as a keybind, but throws an undefined-var
;; error the moment other Lisp code (e.g. a package) calls it as a
;; regular function from within another form.
(define editor-delete-next-char (lambda () (begin
                                            (move-cursor "right")
                                            (editor-delete-previous-char)
                                            )
                                  )
  )


(define (char-between ch lo hi)
  (if (char>=? ch lo)
      (char<=? ch hi)
      #f))

(define (alphanumericp ch)
  (if ch
      (if (char-between ch #\a #\z)
          #t
          (if (char-between ch #\A #\Z)
              #t
              (if (char-between ch #\0 #\9)
                  #t
                  #f)))
      #f))


(define (word-char-p ch)
  (if (alphanumericp ch)
      #t
      #f))

(define editor-move-to-end-of-word (lambda () (
  (if (word-char-p (editor-read-char))
	((move-cursor "right")
	(editor-move-to-end-of-word))
    ))
                                     ))

;; Counts the run of leading spaces on `line`, starting the scan at `i`.
(define (leading-space-count line i)
  (if (and (< i (string-length line)) (char=? (string-ref line i) #\space))
      (leading-space-count line (+ i 1))
      i))

(define (insert-n-spaces n)
  (if (< 0 n)
      (begin
        (editor-insert-char " ")
        (insert-n-spaces (- n 1)))
      #f))

;; Auto-indent: match the indentation of the line the cursor is leaving.
(define enter-and-tab
  (lambda ()
    (let ((indent (leading-space-count (line-text (cursor-row)) 0)))
      (editor-insert-new-line)
      (insert-n-spaces indent))))

(map-key "CTRL-x" '(editor-set-prefix "user") "no-prefix")
(map-key "ALT-x" execute-command "no-prefix")
(map-key "CTRL-g" '(editor-set-prefix "no-prefix") "user")
(map-key "CTRL-c" editor-quit "user")
(map-key "CTRL-s" editor-save "user")
(map-key "ARROW-UP" '(move-cursor "up") "no-prefix")
(map-key "ARROW-DOWN" '(move-cursor "down") "no-prefix")
(map-key "ARROW-RIGHT" '(move-cursor "right") "no-prefix")
(map-key "ARROW-LEFT" '(move-cursor "left") "no-prefix")
(map-key "ENTER" enter-and-tab "no-prefix")
(map-key "CTRL-a" move-cursor-beg-line "no-prefix")
(map-key "CTRL-e" move-cursor-end-line "no-prefix")
(map-key "BACKSPACE" editor-delete-previous-char "no-prefix")
(map-key "DEL" editor-delete-next-char "no-prefix")
(map-key "PAGE-UP" move-cursor-page-up "no-prefix")
(map-key "PAGE-DOWN" move-cursor-page-down "no-prefix")
(map-key "f" editor-open-file "user")
(map-key "c" editor-open-config "user")
(map-key "TAB" editor-insert-tab "no-prefix")
(map-key "CTRL-s" buffer-find "no-prefix")
(map-key "CTRL-r" buffer-find-reverse "no-prefix")
(map-key "ARROW-RIGHT" editor-switch-next-buffer "user")
(map-key "b" editor-switch-buffer-popup "user")
(map-key "\"" editor-split-screen-vertical "user")
(map-key "o" editor-switch-next-pane "user")
(map-key "*" editor-unify-panes "user")
(map-key "CTRL-y" editor-paste "no-prefix")
(map-key "CTRL-k" editor-cut-end-line "no-prefix")
(map-key "CTRL-w" editor-copy-end-line "no-prefix")
(map-key "a" editor-move-cursor-beg-buffer "user")
(map-key "z" editor-move-cursor-end-buffer "user")
(map-key "t" editor-open-terminal "user")
(map-key "ESCAPE" '(editor-set-prefix "no-prefix") "term")
