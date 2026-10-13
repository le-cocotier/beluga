;; MACROS

(define TAB-LENGTH 2)
(define QUIT-TIMES 1)
(define LSP #t)

;; Show a line-number gutter. Width follows the file's line count (min 3
;; digits), and the cursor's own line is drawn brighter than the rest.
(define LINE-NUMBERS #t)

;; which-key style help. With a key prefix pending (e.g. after CTRL-x), the
;; keys available under it appear at the bottom of the screen after a short
;; pause — long enough that fluent chords like CTRL-x CTRL-s don't flash it.
;; At the M-x prompt it lists the commands matching what you've typed.
;; Both settings are read defensively, so a config predating them still
;; loads; omitting either leaves the feature on.
(define WHICH-KEY #t)

;; Show the signature of the function being called, with the current
;; argument highlighted, as soon as "(" or "," is typed in an LSP buffer.
;; CTRL-x p s shows it on demand either way. Omitting this leaves it on.
(define LSP-SIGNATURE-HELP #t)

;; Prefixes need to exist before anything below tries to (map-key ... "user")
;; or "term" — including packages, which load next. A package that tried to
;; bind under a not-yet-registered prefix would silently fall back to
;; "no-prefix" instead (map-key resolves the prefix name to an id right
;; away, at load time, not when the key is actually pressed).
(add-prefix "user")
(add-prefix "term")
(add-prefix "project")
(add-prefix "config")

;; PACKAGES
;;
;; Bundled packages come with beluga (packages/ in its repo) and install.sh
;; puts them in ~/.beluga/packages/ — all of the ones named in this file.
;; A package that isn't there (your own, or a third-party one: git clone it
;; into ~/.beluga/packages/<name>) makes (add-package ...) a silent no-op.
(add-package "smart_delimiters")
(add-package "outline")
(add-package "explorer")
(add-package "dired")    ; CTRL-x d, CTRL-x CTRL-j — and CTRL-x c p below
(add-package "git")      ; CTRL-x g: status, stage, commit, push/pull, log, branches

;; Themes: exactly one should be loaded (the second one just overwrites
;; whatever colors the first one set). Fork either as a starting point
;; for your own — see packages/theme-dark/init.lisp for the interface.
(add-package "theme-dark")
;; (add-package "theme-light")

;; Your own modifications, kept out of this file so an update can replace it
;; without erasing them: ~/.beluga/packages/personal-config/init.lisp,
;; created once by install.sh and never overwritten. Loaded here — after the
;; theme so its colors win, before the key bindings below so its map-keys win.
(add-package "personal-config")

;; Per-language packages: each registers its own
;; (lsp-server "ext" "language-id" "command" "arg1" ...) — which spawns
;; that language server the first time a matching file is opened — plus
;; a CTRL-d comment-toggle keybind. See packages/<name>/init.lisp. All
;; bundled, so enabling one is just uncommenting it (or adding the line to
;; personal-config). Calling (lsp-server ...) directly here also works fine
;; for a language with no package of its own yet.
;; (add-package "c")
;; (add-package "python")   ; needs pyright-langserver on PATH
;; (add-package "rust")     ; needs rust-analyzer on PATH
;; (add-package "php")      ; needs intelephense on PATH — no PHP install required
;; (add-package "latex")    ; digestif ships with TeX Live; texlab is a one-line swap
;; (add-package "yaml")     ; needs yaml-language-server on PATH
;; (add-package "nodejs")   ; needs typescript-language-server + typescript@5
;; (add-package "angular")  ; needs @angular/language-server — Angular's .ts
;;                          ; AND .html template, one shared server. Loads
;;                          ; instead of "nodejs", not alongside it — both
;;                          ; register "ts" and only the first one wins it.
;; (add-package "lisp")     ; no server needed — highlighting + comment toggle only
;;
;; Note on CTRL-d: every language package binds it under "no-prefix", and
;; the FIRST binding registered wins, so only one comment toggle is ever
;; reachable. c/python/rust each hardcode one comment prefix; latex/yaml/
;; nodejs/angular instead dispatch on the file's extension and handle all
;; of these languages. Move whichever of those you enable up above the
;; c/python/rust add-package lines if you want CTRL-d to do the right
;; thing in every file type.

;; HOOKS
;;
;; (add-hook "event-name" fn) runs fn whenever that event fires. fn must be
;; a lambda or a symbol bound to one (not the '(quoted-list) form map-key
;; uses). Currently fired events:
;;   "buffer-opened" (filename buffer-id) - a file finished loading
;;   "before-save"   (filename)           - about to write buffer to disk
;;   "after-save"    (filename)           - buffer was just written
;;   "lsp-ready"     ()                   - LSP handshake completed
;;   "buffer-killed" (name buffer-id)     - a buffer was killed (CTRL-x k)
;;
;; Example: log every save to the status bar.
;; (add-hook "after-save" (lambda (filename) (editor-set-prefix "no-prefix")))

;; Format-on-save. OFF by default and deliberately so: the server formats
;; to ITS style, not yours (clangd defaults to 2-space LLVM style), so
;; enabling this rewrites the indentation of every file you save. Turn it
;; on only if your project has a .clang-format the server will pick up.
;; CTRL-x CTRL-f formats on demand either way.
;; (add-hook "before-save" (lambda (filename) (lsp-format-buffer)))


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

;; Move the cursor forward to just past the end of a word:
;;   - sitting on a word character  -> end of that word;
;;   - sitting on anything else     -> skip the non-word run (across line
;;     breaks) and stop at the end of the next word.
;; Both loops step one column at a time. (editor-read-char) is #f at end
;; of line and word-char-p is #f for #f, so each loop stops there; the
;; skip loop only crosses onto the next line while one exists, so this
;; still terminates at the very end of the buffer.

(define (editor--consume-word)
  (if (word-char-p (editor-read-char))
      (begin (move-cursor "right") (editor--consume-word))
      #f))

(define (editor--skip-to-word)
  (let ((ch (editor-read-char)))
    (if ch
        (if (word-char-p ch)
            #f
            (begin (move-cursor "right") (editor--skip-to-word)))
        ;; end of line — cross to the next one if there is one
        (if (< (cursor-row) (- (buffer-line-count) 1))
            (begin (move-cursor "right") (editor--skip-to-word))
            #f))))

(define editor-move-to-end-of-word
  (lambda ()
    (begin
      (editor--skip-to-word)
      (editor--consume-word))))

;; Do the same but backward
(define (editor--consume-word-backward)
    (if (word-char-p (editor-read-char))
        (begin (move-cursor "left") (editor--consume-word-backward)) #f
    )
)

(define (editor--skip-to-word-backward)
    (let ((ch (editor-read-char)))
        (if ch
            (if (word-char-p ch)
                #f
                (begin (move-cursor "left") (editor--skip-to-word-backward)))
            (if (< (cursor-row) (- (buffer-line-count) 1))
                 (begin (move-cursor "left") (editor--skip-to-word-backward)) #f
            )
        )    
    )
)

(define editor-move-to-begin-of-word
  (lambda ()
    (begin
      (editor--skip-to-word-backward)
      (editor--consume-word-backward))))

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

;; A live-filtering buffer switcher — same primitive CTRL-O's outline and
;; CTRL-P's file finder are each built on (editor-list-popup + #t), just
;; over (list-buffers) instead of a file's symbols or the project's file
;; list. Labels are the exact same "<id>: <filename>[ *]" text
;; editor-switch-buffer-popup (buffer_switcher.c) already draws, so this
;; looks identical to it — the only difference is being able to type to
;; narrow instead of only arrowing through every open buffer.
(define (editor-buffer-picker)
  (editor-list-popup "Buffers"
                      (list-buffers)
                      (lambda (id) (editor-switch-to-buffer id))
                      '()
                      #t))

;; Global
(map-key "CTRL-g" '(editor-set-prefix "no-prefix") "*")

;; no-prefix
(map-key "CTRL-x" '(editor-set-prefix "user") "no-prefix")
(map-key "ALT-x" execute-command "no-prefix")
(map-key "ARROW-UP" '(move-cursor "up") "no-prefix")
(map-key "ARROW-DOWN" '(move-cursor "down") "no-prefix")
(map-key "ARROW-RIGHT" '(move-cursor "right") "no-prefix")
(map-key "ARROW-LEFT" '(move-cursor "left") "no-prefix")
;; Alt/Option + Right — jump to the end of the next word. Recognised from
;; ESC[1;3C, ESC-ESC[C and ESC-f, so it works whether the terminal sends
;; Option+Right as a CSI-modifier arrow or (Terminal.app / iTerm default)
;; as readline's M-f.
(map-key "ALT-ARROW-RIGHT" editor-move-to-end-of-word "no-prefix")
(map-key "ALT-ARROW-LEFT" editor-move-to-begin-of-word "no-prefix")
(map-key "ENTER" enter-and-tab "no-prefix")
(map-key "CTRL-a" move-cursor-beg-line "no-prefix")
(map-key "CTRL-e" move-cursor-end-line "no-prefix")
(map-key "BACKSPACE" editor-delete-previous-char "no-prefix")
(map-key "DEL" editor-delete-next-char "no-prefix")
(map-key "PAGE-UP" move-cursor-page-up "no-prefix")
(map-key "PAGE-DOWN" move-cursor-page-down "no-prefix")
(map-key "TAB" editor-insert-tab "no-prefix")
(map-key "CTRL-s" buffer-find "no-prefix")
(map-key "CTRL-r" buffer-find-reverse "no-prefix")
;; Fuzzy project-file picker: type to narrow every file in the project
;; (git ls-files / find), ENTER opens the highlight. CTRL-p / CTRL-n also
;; move the selection while it's open.
(map-key "CTRL-p" find-file "no-prefix")
(map-key "CTRL-y" editor-paste "no-prefix")
(map-key "CTRL-k" editor-cut-end-line "no-prefix")
(map-key "CTRL-l" editor-copy-end-line "no-prefix")
(map-key "CTRL-SPACE" editor-set-mark "no-prefix")
(map-key "CTRL-DEL" editor-kill-region "no-prefix")
(map-key "CTRL-w" editor-cut-region "no-prefix")

;; user
(map-key "p" '(editor-set-prefix "project") "user")
(map-key "CTRL-c" editor-quit "user")
(map-key "CTRL-s" editor-save "user")
(map-key "f" editor-open-file "user")
(map-key "c" '(editor-set-prefix "config") "user")
(map-key "ARROW-RIGHT" editor-switch-next-buffer "user")
(map-key "b" editor-buffer-picker "user")
(map-key "k" editor-kill-buffer "user")
(map-key "\"" editor-split-screen-vertical "user")
(map-key "%" editor-split-screen-horizontal "user")
(map-key "o" editor-switch-next-pane "user")
(map-key "*" editor-unify-panes "user")
(map-key "a" editor-move-cursor-beg-buffer "user")
(map-key "z" editor-move-cursor-end-buffer "user")
(map-key "t" editor-open-terminal "user")
(map-key "w" editor-copy-region "user")
(map-key "u" editor-undo "user")
(map-key "U" editor-redo "user")
(map-key "s" project-search "user")
(if (app-debug?) (map-key "m" memory-inspector "user"))

;; config
(map-key "c" editor-open-config "config")
(map-key "p" '(dired-open "~/.beluga/packages") "config")
(map-key "m" '(editor-open-file-path "~/.beluga/packages/personal-config/init.lisp") "config")
;; project
(map-key "d" lsp-definition "project")
(map-key "f" lsp-references "project")   ; listed in *search*, ENTER jumps
(map-key "e" lsp-diagnostics "project")  ; every error/warning of the open files
(map-key "h" lsp-hover "project")
(map-key "s" lsp-signature-help "project")
(map-key "a" lsp-code-action "project")
(map-key "r" lsp-rename "project")
(map-key "z" lsp-format-buffer "project")

;; term
(map-key "ESCAPE" '(editor-set-prefix "no-prefix") "term")

;; search
(map-key "ENTER" project-search-goto "search")
(map-key "ARROW-UP" '(move-cursor "up") "search")
(map-key "ARROW-DOWN" '(move-cursor "down") "search")
(map-key "PAGE-UP" move-cursor-page-up "search")
(map-key "PAGE-DOWN" move-cursor-page-down "search")
(map-key "CTRL-x" '(editor-set-prefix "user") "search")

;; memory (debug builds only — CTRL-x m to open, r refresh, s sort, w export, CTRL-x to leave)
(if (app-debug?)
  (begin
    (map-key "ARROW-UP" '(move-cursor "up") "memory")
    (map-key "ARROW-DOWN" '(move-cursor "down") "memory")
    (map-key "PAGE-UP" move-cursor-page-up "memory")
    (map-key "PAGE-DOWN" move-cursor-page-down "memory")
    (map-key "r" memory-inspector-refresh "memory")
    (map-key "s" memory-inspector-sort "memory")
    (map-key "w" memory-inspector-export "memory")
    (map-key "CTRL-x" '(editor-set-prefix "user") "memory")))

