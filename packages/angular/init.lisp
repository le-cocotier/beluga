;; ─── Angular / TypeScript ────────────────────────────────────────────────────
;; (add-package "angular") — registers the Angular Language Service
;; (ngserver) for both a component's TypeScript and its HTML template,
;; plus a CTRL-d comment toggle and syntax highlighting for each.
;;
;; Needs @angular/language-server on PATH:
;;   `npm install -g @angular/language-server`
;;
;; ngserver is a superset of a plain TypeScript server for an Angular
;; workspace — it wraps tsserver itself, so .ts files still get ordinary
;; TypeScript completion/diagnostics/go-to-definition/rename, PLUS
;; template type-checking and completion inside .html files (and inline
;; `template: \`...\`` strings) that a plain typescript-language-server
;; knows nothing about: *ngIf/*ngFor, property/event bindings, pipes,
;; two-way [(ngModel)], the component's own inputs/outputs, all resolved
;; against the TypeScript types they're actually bound to.
;;
;; "ts" and "html" are registered against the exact same command
;; ("ngserver"), so opening both a component's .ts and its .html share
;; one running server instance instead of each starting their own — a
;; client is per command (lspClientForConfig() in buffer.c compares
;; argv[0]). Load THIS package instead of "nodejs" for Angular work, not
;; alongside it — both claim "ts", and whichever add-package ran first
;; wins that extension.
;; Non-Angular TypeScript/JS projects should keep using "nodejs".
;;
;; --ngProbeLocations/--tsProbeLocations tell ngserver where to find
;; @angular/language-service and typescript in your project's own
;; node_modules — VSCode's Angular extension resolves these from the
;; workspace root, but beluga's (lsp-server ...) argv is fixed at
;; package-load time with no per-project templating, so this hardcodes
;; the relative path "node_modules" instead. That resolves correctly as
;; long as beluga is started from the project root (the ordinary way to
;; use it — the "Project root:" prompt only sets the LSP rootUri, it
;; doesn't chdir the server process; see lspStart() in completion.c). A
;; monorepo where the Angular app doesn't sit at the repo's own root
;; would need this changed to that app's own node_modules path.

(lsp-server "ts" "typescript"
            "ngserver" "--ngProbeLocations" "node_modules"
            "--tsProbeLocations" "node_modules" "--stdio")
(lsp-server "html" "html"
            "ngserver" "--ngProbeLocations" "node_modules"
            "--tsProbeLocations" "node_modules" "--stdio")

;; ─── Shared string helpers ──────────────────────────────────────────────────
;; Self-contained, like every other package (there's no import between
;; them — see theme-dark/init.lisp's own note on forking a package as a
;; starting point). Same recursive style as nodejs/outline: this
;; interpreter has no built-in string-search primitive.

(define (angular--starts-with? s prefix)
  (let ((s-len (string-length s)) (p-len (string-length prefix)))
    (and (>= s-len p-len) (string=? (substring s 0 p-len) prefix))))

(define (angular--ends-with? s suffix)
  (let ((s-len (string-length s)) (suf-len (string-length suffix)))
    (and (>= s-len suf-len)
         (string=? (substring s (- s-len suf-len) s-len) suffix))))

(define (angular--last-index-of s ch i best)
  (if (< i (string-length s))
      (angular--last-index-of s ch (+ i 1)
                              (if (char=? (string-ref s i) ch) i best))
      best))

;; The current buffer's extension, lowercased, or "" for a path with no
;; extension. The dot has to come after the last slash, or a file called
;; "notes" inside "~/v1.2/" would come out with the extension "2/notes".
(define (angular--extension)
  (let* ((path (buffer-filename))
         (dot (angular--last-index-of path #\. 0 -1))
         (slash (angular--last-index-of path #\/ 0 -1)))
    (if (and (>= dot 0) (> dot slash))
        (string-downcase (substring path (+ dot 1) (string-length path)))
        "")))

(define (angular--insert-string s i)
  (if (< i (string-length s))
      (begin
        (editor-insert-char (string (string-ref s i)))
        (angular--insert-string s (+ i 1)))))

;; Plain recursion rather than (dotimes ...) — see nodejs/init.lisp's note
;; on why: dotimes' body only reliably calls raw C builtins here, not
;; user-defined lambdas like editor-delete-next-char.
(define (angular--delete-n-forward n)
  (if (< 0 n)
      (begin
        (editor-delete-next-char)
        (angular--delete-n-forward (- n 1)))))

(define (angular--delete-n-backward n)
  (if (< 0 n)
      (begin
        (editor-delete-previous-char)
        (angular--delete-n-backward (- n 1)))))

;; ─── Comment toggle ─────────────────────────────────────────────────────────
;; TypeScript gets the same "// " leading-prefix toggle as every other
;; C-like language package (see nodejs/init.lisp) — comments and
;; uncomments at column 0, not in front of the code, on the theory that
;; this is a small enough wart to trade for staying dead simple.
;;
;; HTML has no per-line comment syntax at all — only the wrapping
;; <!-- ... -->, so this is a genuinely different toggle: wrap the whole
;; line's content in "<!-- " / " -->" (also at column 0), or strip it back
;; out if it's already exactly that shape. This is the first wrap-style
;; toggle in any package here (every other language's is a leading
;; prefix); it only recognizes the exact "<!-- "/" -->" markers this same
;; toggle writes, the same way the prefix toggles only recognize their
;; own exact prefix.

(define angular--ts-prefix-styles
  (list (cons "ts" "// ") (cons "tsx" "// ") (cons "mts" "// ") (cons "cts" "// ")))

(define (angular--lookup key styles)
  (if (null? styles)
      ""
      (if (string=? key (car (car styles)))
          (cdr (car styles))
          (angular--lookup key (cdr styles)))))

(define (angular--html-toggle-comment)
  (let* ((row (cursor-row))
         (text (line-text row)))
    (if (and (angular--starts-with? text "<!-- ") (angular--ends-with? text " -->"))
        (begin
          ;; Strip the suffix first: it's anchored to the end, so removing
          ;; it doesn't shift where the prefix starts. Stripping the
          ;; prefix first would have the opposite problem.
          (move-cursor-end-line)
          (angular--delete-n-backward 4)
          (move-cursor-beg-line)
          (angular--delete-n-forward 5))
        (begin
          ;; Same ordering reasoning in reverse: append the suffix while
          ;; still anchored at the (pre-edit) end, then go add the prefix.
          (move-cursor-end-line)
          (angular--insert-string " -->" 0)
          (move-cursor-beg-line)
          (angular--insert-string "<!-- " 0)))))

(define (angular--ts-toggle-comment)
  (let* ((prefix (angular--lookup (angular--extension) angular--ts-prefix-styles))
         (row (cursor-row)))
    (if (string=? prefix "")
        #f
        (begin
          (move-cursor-beg-line)
          (if (angular--starts-with? (line-text row) prefix)
              (angular--delete-n-forward (string-length prefix))
              (angular--insert-string prefix 0))))))

(define (angular-toggle-comment)
  (if (string=? (angular--extension) "html")
      (angular--html-toggle-comment)
      (angular--ts-toggle-comment)))

;; Bound under "no-prefix" (not "user") so this doesn't depend on load
;; order relative to (add-prefix "user") in the master config — same
;; reasoning as nodejs/init.lisp.
(map-key "CTRL-d" angular-toggle-comment "no-prefix")

;; ─── Syntax highlighting ────────────────────────────────────────────────────

(define angular--ts-keywords
  (list "if" "else" "while" "do" "for" "switch" "case" "default" "break"
        "continue" "return" "function" "let" "const" "var" "class" "extends"
        "implements" "interface" "type" "enum" "export" "import" "from" "as"
        "new" "delete" "typeof" "instanceof" "in" "of" "try"
        "catch" "finally" "throw" "async" "await" "yield" "super" "this"
        "static" "readonly" "abstract" "namespace" "declare" "public"
        "private" "protected"))

(define angular--ts-types
  (list "string" "number" "boolean" "any" "void" "never" "unknown" "object"
        "symbol" "bigint" "null" "undefined" "true" "false" "Array"
        "Promise" "Record" "Partial" "Readonly"
        ;; Angular-specific vocabulary, so it visibly stands out from
        ;; plain TypeScript rather than blending into "Array"/"Promise".
        "Component" "Injectable" "NgModule" "Directive" "Pipe"
        "Input" "Output" "ViewChild" "ViewChildren" "ContentChild"
        "ContentChildren" "HostListener" "HostBinding"
        "OnInit" "OnDestroy" "OnChanges" "AfterViewInit"
        "EventEmitter" "Observable" "Subject" "Subscription"))

(define (angular--add-keywords ext words)
  (if (null? words)
      #f
      (begin
        (syntax-add-keyword ext (car words))
        (angular--add-keywords ext (cdr words)))))

(define (angular--add-types ext words)
  (if (null? words)
      #f
      (begin
        (syntax-add-type ext (car words))
        (angular--add-types ext (cdr words)))))

;; "//" line comments, "/* */" block comments, three quote styles — same
;; simplification as nodejs/init.lisp (a template literal's ${...}
;; interpolation isn't specially recognized, the whole backtick span is
;; one string).
(define (angular--register-ts ext)
  (syntax-define-language ext "//" "/*" "*/" "\"'`")
  (angular--add-keywords ext angular--ts-keywords)
  (angular--add-types ext angular--ts-types))

(angular--register-ts "ts")
(angular--register-ts "tsx")
(angular--register-ts "mts")
(angular--register-ts "cts")

;; HTML has no line comment, so that argument is "". Word-list highlighting
;; is a poor fit for markup in general (there's no real distinction here
;; between a tag name and plain text that happens to match one), but it's
;; the only primitive this highlighter offers, and it's genuinely useful
;; for the one thing that makes an Angular template not-quite-plain-HTML:
;; making its own directives/bindings — ngIf, ngFor, formControlName, and
;; so on — visually pop out from the surrounding markup. Tag names go in
;; as keywords, Angular-specific attribute/directive names as types, so
;; the two are at least visually distinguishable from each other.
(define angular--html-keywords
  (list "div" "span" "a" "button" "input" "form" "label" "select" "option"
        "textarea" "ul" "li" "ol" "table" "tr" "td" "th" "thead" "tbody"
        "img" "svg" "path" "section" "header" "footer" "nav" "main"
        "article" "aside" "h1" "h2" "h3" "h4" "h5" "h6" "p" "br" "hr"
        "ng-container" "ng-template" "ng-content" "router-outlet"))

(define angular--html-types
  (list "ngIf" "ngFor" "ngSwitch" "ngSwitchCase" "ngSwitchDefault"
        "ngClass" "ngStyle" "ngModel" "ngSubmit" "ngValue" "ngTemplateOutlet"
        "formGroup" "formControlName" "formArrayName" "formGroupName"
        "routerLink" "routerLinkActive" "class" "id" "style"))

(syntax-define-language "html" "" "<!--" "-->" "\"'")
(angular--add-keywords "html" angular--html-keywords)
(angular--add-types "html" angular--html-types)
