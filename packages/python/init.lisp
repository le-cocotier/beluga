;; ─── Python ─────────────────────────────────────────────────────────────────
;; (add-package "python") — registers pyright for .py files, plus a
;; CTRL-d line-comment toggle so the package demonstrates more than just
;; LSP wiring. Fork/extend with whatever else your Python workflow needs.
;;
;; Needs pyright-langserver on PATH: `npm install -g pyright`.

(lsp-server "py" "python" "pyright-langserver" "--stdio")

(define (python--line-starts-with? s prefix)
  (let ((s-len (string-length s)) (p-len (string-length prefix)))
    (and (>= s-len p-len) (string=? (substring s 0 p-len) prefix))))

(define (python--insert-string s i)
  (if (< i (string-length s))
      (begin
        (editor-insert-char (string (string-ref s i)))
        (python--insert-string s (+ i 1)))))

;; Plain recursion rather than (dotimes ...) — dotimes' body currently
;; only reliably calls raw C builtins, not user-defined Lisp lambdas like
;; editor-delete-next-char, so it silently stops after the first
;; iteration here.
(define (python--delete-n-chars n)
  (if (< 0 n)
      (begin
        (editor-delete-next-char)
        (python--delete-n-chars (- n 1)))))

(define (python-toggle-comment)
  (let ((row (cursor-row)))
    (move-cursor-beg-line)
    (if (python--line-starts-with? (line-text row) "# ")
        (python--delete-n-chars (string-length "# "))
        (python--insert-string "# " 0))))

;; CTRL-d under "no-prefix" — bound here (not "user") so this package
;; doesn't depend on load order relative to (add-prefix "user") in the
;; master config. The last argument scopes it to this language's files, so
;; every language package can bind its own CTRL-d side by side.
(map-key "CTRL-d" python-toggle-comment "no-prefix" "py")

;; ─── Syntax highlighting ────────────────────────────────────────────────────
;; syntax-define-language takes one block-comment open/close pair, and
;; Python doesn't really have one — a triple-quoted string used as a
;; standalone statement is idiomatic "block comment" convention, but it's
;; still just a string to the language itself, so "" / "" here correctly
;; leaves block comments off rather than mismodeling triple quotes as
;; C-style /* */ delimiters they aren't.

(define python-keywords
  (list "if" "elif" "else" "while" "for" "in" "break" "continue" "return"
        "def" "class" "import" "from" "as" "try" "except" "finally" "raise"
        "with" "lambda" "global" "nonlocal" "pass" "yield" "assert" "del"
        "is" "not" "and" "or" "async" "await" "match" "case" "self"))

(define python-types
  (list "int" "float" "str" "bool" "list" "dict" "tuple" "set" "frozenset"
        "bytes" "object" "None" "True" "False"))

(define (python--add-keywords words)
  (if (null? words)
      #f
      (begin
        (syntax-add-keyword "py" (car words))
        (python--add-keywords (cdr words)))))

(define (python--add-types words)
  (if (null? words)
      #f
      (begin
        (syntax-add-type "py" (car words))
        (python--add-types (cdr words)))))

;; "#" line comments, no block comment, both quote styles open a string —
;; f-strings and raw strings still start with the same quote character, so
;; the "f"/"r" prefix just renders as a plain identifier right before it.
(syntax-define-language "py" "#" "" "" "\"'")
(python--add-keywords python-keywords)
(python--add-types python-types)
