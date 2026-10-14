;; ─── Node.js / JavaScript / TypeScript ──────────────────────────────────────
;; (add-package "nodejs") — registers typescript-language-server for the
;; JavaScript and TypeScript family, plus a CTRL-d comment toggle and
;; CTRL-x j to drop in a labelled console.log.
;;
;; Needs typescript-language-server, and a TypeScript 5:
;;   `npm install -g typescript@5 typescript-language-server`
;;
;; The version pin matters as of TypeScript 7, which is the Go rewrite and
;; presents a different tsserver: typescript-language-server 6 refuses it
;; outright, failing initialize with "The TypeScript of the workspace
;; (TypeScript 7.x) ..." and exiting, so you get a server that never
;; answers rather than an obvious error. A plain `npm install -g
;; typescript` gets you 7 today. Drop the pin once the language server
;; supports it.
;;
;; The server looks for its TypeScript in the workspace first, so a project
;; with typescript in its own node_modules uses that one and ignores the
;; global install — which is usually what you want, and also means a
;; project pinned to TypeScript 7 will fail the same way.
;;
;; The server handles plain JavaScript as well as TypeScript — it's the
;; same tsserver underneath either way, so a Node project with no
;; tsconfig.json still gets completion, diagnostics, go-to-definition and
;; rename. Each extension is registered with its own LSP language id
;; (javascript / javascriptreact / typescript / typescriptreact), which is
;; what the server uses to decide how to parse the file: register .tsx as
;; plain "typescript" and JSX syntax comes back as errors.

(lsp-server "js"  "javascript"      "typescript-language-server" "--stdio")
(lsp-server "mjs" "javascript"      "typescript-language-server" "--stdio")
(lsp-server "cjs" "javascript"      "typescript-language-server" "--stdio")
(lsp-server "jsx" "javascriptreact" "typescript-language-server" "--stdio")
(lsp-server "ts"  "typescript"      "typescript-language-server" "--stdio")
(lsp-server "mts" "typescript"      "typescript-language-server" "--stdio")
(lsp-server "cts" "typescript"      "typescript-language-server" "--stdio")
(lsp-server "tsx" "typescriptreact" "typescript-language-server" "--stdio")

;; .json is deliberately not registered here: tsserver isn't a JSON
;; language server. If you want schema-aware JSON, that's a separate
;; server — `npm install -g vscode-langservers-extracted`, then
;; (lsp-server "json" "json" "vscode-json-language-server" "--stdio").
;; Remember only one server runs per session, whichever matches first.

;; ─── Comment toggle ─────────────────────────────────────────────────────────
;; Dispatches on the file's extension rather than hardcoding "// ". The
;; c/python/rust packages each bind CTRL-d under "no-prefix" with a
;; single-language toggle, and the last binding registered wins — so with
;; several loaded, exactly one is reachable. Load this package after those
;; if you want the extension-aware behavior to be the one that wins.

(define nodejs--comment-styles
  (list (cons "js" "// ") (cons "mjs" "// ") (cons "cjs" "// ") (cons "jsx" "// ")
        (cons "ts" "// ") (cons "mts" "// ") (cons "cts" "// ") (cons "tsx" "// ")
        (cons "c" "// ") (cons "h" "// ") (cons "cpp" "// ") (cons "hpp" "// ")
        (cons "rs" "// ") (cons "css" "// ") (cons "scss" "// ")
        (cons "py" "# ") (cons "yaml" "# ") (cons "yml" "# ")
        (cons "toml" "# ") (cons "sh" "# ")
        (cons "tex" "% ") (cons "sty" "% ") (cons "cls" "% ") (cons "bib" "% ")
        (cons "lisp" ";; ")))

(define (nodejs--lookup key styles)
  (if (null? styles)
      ""
      (if (string=? key (car (car styles)))
          (cdr (car styles))
          (nodejs--lookup key (cdr styles)))))

;; Index of the last `ch` in `s`, or -1. Plain recursion, like the other
;; packages — the interpreter has no string-search primitive.
(define (nodejs--last-index-of s ch i best)
  (if (< i (string-length s))
      (nodejs--last-index-of s ch (+ i 1)
                             (if (char=? (string-ref s i) ch) i best))
      best))

;; The current buffer's extension, lowercased, or "" for a path with no
;; extension at all. The dot has to come after the last slash, or a file
;; called "notes" inside "~/v1.2/" would come out with the extension "2/notes".
(define (nodejs--extension)
  (let* ((path (buffer-filename))
         (dot (nodejs--last-index-of path #\. 0 -1))
         (slash (nodejs--last-index-of path #\/ 0 -1)))
    (if (and (>= dot 0) (> dot slash))
        (string-downcase (substring path (+ dot 1) (string-length path)))
        "")))

(define (nodejs--line-starts-with? s prefix)
  (let ((s-len (string-length s)) (p-len (string-length prefix)))
    (and (>= s-len p-len) (string=? (substring s 0 p-len) prefix))))

(define (nodejs--insert-string s i)
  (if (< i (string-length s))
      (begin
        (editor-insert-char (string (string-ref s i)))
        (nodejs--insert-string s (+ i 1)))))

;; Plain recursion rather than (dotimes ...) — dotimes' body currently
;; only reliably calls raw C builtins, not user-defined Lisp lambdas like
;; editor-delete-next-char, so it silently stops after the first
;; iteration here.
(define (nodejs--delete-n-chars n)
  (if (< 0 n)
      (begin
        (editor-delete-next-char)
        (nodejs--delete-n-chars (- n 1)))))

;; Comments and uncomments at column 0, same as the c/python/rust toggles.
;; An indented line gets its "// " at the far left rather than in front of
;; the code — valid JavaScript, if not what a formatter would produce.
(define (nodejs-toggle-comment)
  (let* ((prefix (nodejs--lookup (nodejs--extension) nodejs--comment-styles))
         (row (cursor-row)))
    (if (string=? prefix "")
        #f                      ; unknown file type: leave the line alone
        (begin
          (move-cursor-beg-line)
          (if (nodejs--line-starts-with? (line-text row) prefix)
              (nodejs--delete-n-chars (string-length prefix))
              (nodejs--insert-string prefix 0))))))

;; ─── console.log ────────────────────────────────────────────────────────────
;; CTRL-x j opens a new line below and writes a console.log already
;; labelled with the file and line it came from, so a handful of them in
;; one session are still tellable apart in the terminal. Leaves the cursor
;; between the quotes-and-comma and the closing paren, where the thing you
;; actually want to print goes.

(define (nodejs--basename s)
  (let ((slash (nodejs--last-index-of s #\/ 0 -1)))
    (substring s (+ slash 1) (string-length s))))

(define (nodejs-insert-log)
  (let* ((file (nodejs--basename (buffer-filename)))
         ;; +2, not +1: cursor-row is 0-based and the log lands on the line
         ;; *below* this one, so this is the line the statement itself will
         ;; occupy — which is the number you want when you read it back in
         ;; the terminal.
         (line (number->string (+ (cursor-row) 2)))
         (label (string-append "console.log(\"" file ":" line " \", )")))
    (begin
      (move-cursor-end-line)
      (editor-insert-new-line)
      (nodejs--insert-string label 0)
      ;; land inside the call, just before the closing paren
      (move-cursor "left"))))

;; CTRL-d under "no-prefix" — bound here (not "user") so this package
;; doesn't depend on load order relative to (add-prefix "user") in the
;; master config.
(map-key "CTRL-d" nodejs-toggle-comment "no-prefix")
(map-key "j" nodejs-insert-log "user")

;; ─── Syntax highlighting ────────────────────────────────────────────────────
;; One word list shared by every extension in the JS/TS family — the same
;; way the LSP registration above treats them as one language underneath,
;; even though each gets its own syntax-define-language call (one
;; extension per call, same as lsp-server).

(define nodejs-keywords
  (list "if" "else" "while" "do" "for" "switch" "case" "default" "break"
        "continue" "return" "function" "let" "const" "var" "class" "extends"
        "implements" "interface" "type" "enum" "export" "import" "from" "as"
        "new" "delete" "typeof" "instanceof" "in" "of" "try"
        "catch" "finally" "throw" "async" "await" "yield" "super" "this"
        "static" "readonly" "abstract" "namespace" "declare" "public"
        "private" "protected"))

(define nodejs-types
  (list "string" "number" "boolean" "any" "void" "never" "unknown" "object"
        "symbol" "bigint" "null" "undefined" "true" "false" "Array"
        "Promise" "Record" "Partial" "Readonly"))

(define (nodejs--add-keywords ext words)
  (if (null? words)
      #f
      (begin
        (syntax-add-keyword ext (car words))
        (nodejs--add-keywords ext (cdr words)))))

(define (nodejs--add-types ext words)
  (if (null? words)
      #f
      (begin
        (syntax-add-type ext (car words))
        (nodejs--add-types ext (cdr words)))))

;; "//" line comments, "/* */" block comments, and three quote styles — "
;; and ' for plain strings, ` for template literals. Interpolation inside
;; a template literal (${...}) isn't specially recognized: the highlighter
;; treats the whole backtick-to-backtick span as one string, same
;; simplification every other language's string handling here makes.
(define (nodejs--register-syntax ext)
  (syntax-define-language ext "//" "/*" "*/" "\"'`")
  (nodejs--add-keywords ext nodejs-keywords)
  (nodejs--add-types ext nodejs-types))

(nodejs--register-syntax "js")
(nodejs--register-syntax "mjs")
(nodejs--register-syntax "cjs")
(nodejs--register-syntax "jsx")
(nodejs--register-syntax "ts")
(nodejs--register-syntax "mts")
(nodejs--register-syntax "cts")
(nodejs--register-syntax "tsx")
