;; ─── C ──────────────────────────────────────────────────────────────────────
;; (add-package "c") — registers clangd for .c/.h files, plus a CTRL-d
;; line-comment toggle so the package demonstrates more than just LSP
;; wiring. Fork/extend with whatever else your C workflow needs.
;;
;; The debugger (CTRL-x B): gdb 14 or later speaks the Debug Adapter Protocol
;; itself (gdb -i dap); on macOS, where gdb is a pain, lldb-dap from
;; Xcode's command line tools. It isn't on PATH there, so it goes through
;; xcrun; for Homebrew's llvm instead, (dap-adapter "c" "lldb-dap") in
;; personal-config.

(lsp-server "c" "c" "clangd")
(lsp-server "h" "c" "clangd")
;; /tmp and /var are links into /private on macOS: the debug info has the
;; path a file was compiled by, beluga the real one, and lldb only places a
;; breakpoint where they match. The source map makes them the same.
(define c--lldb-launch
  '(("sourceMap" ("/tmp" . "/private/tmp") ("/var" . "/private/var") ("/etc" . "/private/etc"))))
(if (string=? (system-type) "darwin")
    (begin (dap-adapter "c" "xcrun" "lldb-dap" c--lldb-launch)
           (dap-adapter "h" "xcrun" "lldb-dap" c--lldb-launch))
    (begin (dap-adapter "c" "gdb" "-q" "-i" "dap") (dap-adapter "h" "gdb" "-q" "-i" "dap")))

(define (c--line-starts-with? s prefix)
  (let ((s-len (string-length s)) (p-len (string-length prefix)))
    (and (>= s-len p-len) (string=? (substring s 0 p-len) prefix))))

(define (c--insert-string s i)
  (if (< i (string-length s))
      (begin
        (editor-insert-char (string (string-ref s i)))
        (c--insert-string s (+ i 1)))))

;; Plain recursion rather than (dotimes ...) — dotimes' body currently
;; only reliably calls raw C builtins, not user-defined Lisp lambdas like
;; editor-delete-next-char, so it silently stops after the first
;; iteration here.
(define (c--delete-n-chars n)
  (if (< 0 n)
      (begin
        (editor-delete-next-char)
        (c--delete-n-chars (- n 1)))))

(define (c-toggle-comment)
  (let ((row (cursor-row)))
    (move-cursor-beg-line)
    (if (c--line-starts-with? (line-text row) "// ")
        (c--delete-n-chars (string-length "// "))
        (c--insert-string "// " 0))))

;; CTRL-d under "no-prefix" — bound here (not "user") so this package
;; doesn't depend on load order relative to (add-prefix "user") in the
;; master config. The last argument scopes it to this language's files, so
;; every language package can bind its own CTRL-d side by side.
(map-key "CTRL-d" c-toggle-comment "no-prefix" (list "c" "h"))

;; ─── Syntax highlighting ────────────────────────────────────────────────────
;; The exact keyword and type lists beluga's own syntax highlighter used to
;; hardcode in C, before highlighting itself became something packages
;; provide — every language, C included, now goes through the same
;; (syntax-define-language ...) + syntax-add-keyword/syntax-add-type
;; mechanism. Removing this package no longer just loses clangd and the
;; comment toggle; it loses C highlighting too, the same as any other
;; language package being absent already meant no highlighting for it.

(define c-keywords
  (list "if" "else" "while" "for" "do" "switch"
        "case" "break" "#include" "#define" "#if" "#endif" "#ifndef" "continue" "return"
        "goto" "struct" "union" "enum" "typedef" "static"
        "extern" "const" "volatile" "sizeof" "auto" "register"
        "inline" "restrict"))

(define c-types
  (list "int" "char" "float" "double"
        "void" "long" "short" "unsigned"
        "signed" "bool"))

(define (c--add-keywords ext words)
  (if (null? words)
      #f
      (begin
        (syntax-add-keyword ext (car words))
        (c--add-keywords ext (cdr words)))))

(define (c--add-types ext words)
  (if (null? words)
      #f
      (begin
        (syntax-add-type ext (car words))
        (c--add-types ext (cdr words)))))

;; "//" line comments, "/* */" block comments, both '"' and '\'' open a
;; string/char literal — registered separately for .c and .h since
;; syntax-define-language is one extension per call, same as lsp-server.
(define (c--register-syntax ext)
  (syntax-define-language ext "//" "/*" "*/" "\"'")
  (c--add-keywords ext c-keywords)
  (c--add-types ext c-types))

(c--register-syntax "c")
(c--register-syntax "h")
