;; ─── PHP ────────────────────────────────────────────────────────────────────
;; (add-package "php") — registers Intelephense for .php files, plus a
;; CTRL-d comment toggle and CTRL-x v to insert a var_dump() with the
;; line it came from.
;;
;; Needs intelephense on PATH: `npm install -g intelephense`.
;;
;; Intelephense is a pure Node.js implementation — it parses PHP itself
;; rather than shelling out to a php binary, so it works even on a machine
;; with no PHP interpreter installed. It's free for the completion/hover/
;; definition/diagnostics features this package uses; some of its other
;; features (e.g. full workspace-wide rename) are licensed separately, but
;; nothing beluga's LSP integration calls into needs a license.

(lsp-server "php" "php" "intelephense" "--stdio")
(lsp-server-install "intelephense" "any" "npm install -g intelephense")

(define (php--line-starts-with? s prefix)
  (let ((s-len (string-length s)) (p-len (string-length prefix)))
    (and (>= s-len p-len) (string=? (substring s 0 p-len) prefix))))

(define (php--insert-string s i)
  (if (< i (string-length s))
      (begin
        (editor-insert-char (string (string-ref s i)))
        (php--insert-string s (+ i 1)))))

;; Plain recursion rather than (dotimes ...) — dotimes' body currently
;; only reliably calls raw C builtins, not user-defined Lisp lambdas like
;; editor-delete-next-char, so it silently stops after the first
;; iteration here.
(define (php--delete-n-chars n)
  (if (< 0 n)
      (begin
        (editor-delete-next-char)
        (php--delete-n-chars (- n 1)))))

(define (php-toggle-comment)
  (let ((row (cursor-row)))
    (move-cursor-beg-line)
    (if (php--line-starts-with? (line-text row) "// ")
        (php--delete-n-chars (string-length "// "))
        (php--insert-string "// " 0))))

;; CTRL-d under "no-prefix" — bound here (not "user") so this package
;; doesn't depend on load order relative to (add-prefix "user") in the
;; master config. The last argument scopes it to this language's files, so
;; every language package can bind its own CTRL-d side by side.
(map-key "CTRL-d" php-toggle-comment "no-prefix" "php")

;; ─── var_dump ───────────────────────────────────────────────────────────────
;; CTRL-x v opens a new line below and writes a var_dump() already tagged
;; with the line it's on. Unlike the equivalent CTRL-x j in the nodejs
;; package (which has to compute the line number itself, as plain text,
;; since JavaScript has no runtime equivalent), this one uses PHP's own
;; __LINE__ magic constant — evaluated by PHP itself when the line runs,
;; so it stays correct even after the file is edited further and this
;; particular line shifts. Leaves the cursor right after the comma, where
;; the thing you actually want to inspect goes.

(define (php-insert-var-dump)
  (begin
    (move-cursor-end-line)
    (editor-insert-new-line)
    (php--insert-string "var_dump(__LINE__, );" 0)
    ;; land just before the closing "());" — one space back from the end
    (move-cursor "left")
    (move-cursor "left")))

(map-key "v" php-insert-var-dump "user")

;; ─── Syntax highlighting ────────────────────────────────────────────────────
;; PHP had no syntax highlighting at all before this — the highlighter used
;; to be hardcoded to C and nothing else. Registering it here, the same way
;; the "c" package now registers its own keyword/type lists, is what lights
;; up .php files: (syntax-define-language ...) declares the punctuation
;; (comment/string syntax), then syntax-add-keyword/syntax-add-type build up
;; the word list one call at a time.

(define php-keywords
  (list "if" "else" "elseif" "endif" "while" "endwhile" "do" "for" "endfor"
        "foreach" "endforeach" "switch" "endswitch" "case" "default" "break"
        "continue" "return" "function" "fn" "class" "interface" "trait"
        "extends" "implements" "abstract" "final" "public" "private"
        "protected" "static" "const" "new" "clone" "instanceof" "throw"
        "try" "catch" "finally" "namespace" "use" "global" "echo" "print"
        "require" "require_once" "include" "include_once" "match" "yield"
        "and" "or" "xor" "isset" "unset" "empty" "list" "as"))

(define php-types
  (list "int" "float" "string" "bool" "array" "object" "callable" "iterable"
        "void" "mixed" "never" "self" "parent" "null" "true" "false"))

(define (php--add-keywords words)
  (if (null? words)
      #f
      (begin
        (syntax-add-keyword "php" (car words))
        (php--add-keywords (cdr words)))))

(define (php--add-types words)
  (if (null? words)
      #f
      (begin
        (syntax-add-type "php" (car words))
        (php--add-types (cdr words)))))

;; "//" line comments, "/* */" block comments (both real PHP — "# " is the
;; other valid line-comment form, but this package's own CTRL-d toggle
;; already standardized on "//", so highlighting matches that rather than
;; recognizing a style the toggle itself never produces), both quote styles
;; open a string.
(syntax-define-language "php" "//" "/*" "*/" "\"'")
(php--add-keywords php-keywords)
(php--add-types php-types)
