;; ─── YAML ───────────────────────────────────────────────────────────────────
;; (add-package "yaml") — registers yaml-language-server for .yaml/.yml,
;; plus a CTRL-d comment toggle.
;;
;; Needs yaml-language-server on PATH (it's a Node program):
;;   `npm install -g yaml-language-server`
;;
;; It gives you completion and validation against whatever schema applies —
;; GitHub Actions workflows, docker-compose, Kubernetes manifests and so on
;; are matched from the SchemaStore catalog by filename, with no extra
;; configuration. Beluga sends no per-language settings, so anything that
;; needs an explicit schema mapping won't get one; the catalog covers the
;; common cases on its own.

(lsp-server "yaml" "yaml" "yaml-language-server" "--stdio")
(lsp-server "yml" "yaml" "yaml-language-server" "--stdio")

;; ─── Comment toggle ─────────────────────────────────────────────────────────
;; Dispatches on the file's extension rather than hardcoding "# ". The
;; c/python/rust packages each bind CTRL-d under "no-prefix" with a
;; single-language toggle, and the first binding registered wins — so with
;; several loaded, exactly one is reachable, and a hardcoded one cheerfully
;; puts "// " at the top of a YAML file. Load this package before those if
;; you want the extension-aware behavior to be the one that wins.

(define yaml--comment-styles
  (list (cons "yaml" "# ") (cons "yml" "# ") (cons "toml" "# ") (cons "sh" "# ")
        (cons "py" "# ") (cons "rb" "# ") (cons "conf" "# ")
        (cons "c" "// ") (cons "h" "// ") (cons "cpp" "// ") (cons "hpp" "// ")
        (cons "js" "// ") (cons "jsx" "// ") (cons "mjs" "// ") (cons "cjs" "// ")
        (cons "ts" "// ") (cons "tsx" "// ") (cons "rs" "// ")
        (cons "tex" "% ") (cons "sty" "% ") (cons "cls" "% ") (cons "bib" "% ")
        (cons "lisp" ";; ")))

(define (yaml--lookup key styles)
  (if (null? styles)
      ""
      (if (string=? key (car (car styles)))
          (cdr (car styles))
          (yaml--lookup key (cdr styles)))))

;; Index of the last `ch` in `s`, or -1. Plain recursion, like the other
;; packages — the interpreter has no string-search primitive.
(define (yaml--last-index-of s ch i best)
  (if (< i (string-length s))
      (yaml--last-index-of s ch (+ i 1)
                           (if (char=? (string-ref s i) ch) i best))
      best))

;; The current buffer's extension, lowercased, or "" for a path with no
;; extension at all. The dot has to come after the last slash, or a file
;; called "notes" inside "~/v1.2/" would come out with the extension "2/notes".
(define (yaml--extension)
  (let* ((path (buffer-filename))
         (dot (yaml--last-index-of path #\. 0 -1))
         (slash (yaml--last-index-of path #\/ 0 -1)))
    (if (and (>= dot 0) (> dot slash))
        (string-downcase (substring path (+ dot 1) (string-length path)))
        "")))

(define (yaml--line-starts-with? s prefix)
  (let ((s-len (string-length s)) (p-len (string-length prefix)))
    (and (>= s-len p-len) (string=? (substring s 0 p-len) prefix))))

(define (yaml--insert-string s i)
  (if (< i (string-length s))
      (begin
        (editor-insert-char (string (string-ref s i)))
        (yaml--insert-string s (+ i 1)))))

;; Plain recursion rather than (dotimes ...) — dotimes' body currently
;; only reliably calls raw C builtins, not user-defined Lisp lambdas like
;; editor-delete-next-char, so it silently stops after the first
;; iteration here.
(define (yaml--delete-n-chars n)
  (if (< 0 n)
      (begin
        (editor-delete-next-char)
        (yaml--delete-n-chars (- n 1)))))

;; Comments and uncomments at column 0, same as the c/python/rust toggles.
;; Worth knowing in YAML specifically: a "#" at column 0 comments the line
;; out wherever it sat in the nesting, which is what you want, but the
;; marker won't line up with the indentation of what it's commenting.
(define (yaml-toggle-comment)
  (let* ((prefix (yaml--lookup (yaml--extension) yaml--comment-styles))
         (row (cursor-row)))
    (if (string=? prefix "")
        #f                      ; unknown file type: leave the line alone
        (begin
          (move-cursor-beg-line)
          (if (yaml--line-starts-with? (line-text row) prefix)
              (yaml--delete-n-chars (string-length prefix))
              (yaml--insert-string prefix 0))))))

;; CTRL-d under "no-prefix" — bound here (not "user") so this package
;; doesn't depend on load order relative to (add-prefix "user") in the
;; master config.
(map-key "CTRL-d" yaml-toggle-comment "no-prefix")

;; ─── Syntax highlighting ────────────────────────────────────────────────────
;; YAML has no real keywords in the C sense, but its reserved scalars
;; (true/false/null and their common aliases) read a lot like keywords in
;; practice, so they're registered as "types" — the same bucket the c/php/
;; etc. packages use for built-in constant-ish words.

(define yaml-constants
  (list "true" "false" "null" "yes" "no" "on" "off" "True" "False" "Null"
        "Yes" "No" "On" "Off" "TRUE" "FALSE" "NULL" "YES" "NO" "ON" "OFF"))

(define (yaml--add-types ext words)
  (if (null? words)
      #f
      (begin
        (syntax-add-type ext (car words))
        (yaml--add-types ext (cdr words)))))

;; "#" line comments, no block comment (YAML doesn't have one), both quote
;; styles open a string — registered for both extensions the same way the
;; c package registers .c and .h separately.
(define (yaml--register-syntax ext)
  (syntax-define-language ext "#" "" "" "\"'")
  (yaml--add-types ext yaml-constants))

(yaml--register-syntax "yaml")
(yaml--register-syntax "yml")
