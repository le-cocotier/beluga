;; ─── Lisp ───────────────────────────────────────────────────────────────────
;; (add-package "lisp") — a CTRL-d comment toggle and syntax highlighting
;; for Lisp-family files: .lisp / .cl (Common Lisp), .scm / .ss / .sld
;; (Scheme), .el (Emacs Lisp). Beluga's own config and package files are
;; .lisp, so this lights those up too.
;;
;; Unlike the other language packages, this one registers NO LSP server.
;; There's no Lisp language server that's reliably on a machine the way
;; clangd or pyright are — the dialect-specific ones each need their own
;; runtime installed and only cover their own dialect:
;;
;;   Racket:  (lsp-server "rkt" "racket" "racket-langserver")
;;            — `raco pkg install racket-langserver`
;;   Clojure: (lsp-server "clj" "clojure" "clojure-lsp")
;;            (also .cljs/.cljc/.edn) — https://clojure-lsp.io
;;   Fennel:  (lsp-server "fnl" "fennel" "fennel-ls")
;;
;; Uncomment whichever matches what you write and have installed.

(define (lisp--line-starts-with? s prefix)
  (let ((s-len (string-length s)) (p-len (string-length prefix)))
    (and (>= s-len p-len) (string=? (substring s 0 p-len) prefix))))

(define (lisp--insert-string s i)
  (if (< i (string-length s))
      (begin
        (editor-insert-char (string (string-ref s i)))
        (lisp--insert-string s (+ i 1)))))

;; Plain recursion rather than (dotimes ...) — dotimes' body currently
;; only reliably calls raw C builtins, not user-defined Lisp lambdas like
;; editor-delete-next-char, so it silently stops after the first
;; iteration here.
(define (lisp--delete-n-chars n)
  (if (< 0 n)
      (begin
        (editor-delete-next-char)
        (lisp--delete-n-chars (- n 1)))))

;; ";; " rather than a single ";" — the two-semicolon form is the
;; convention for a comment that sits on its own line (a single ";" is
;; conventionally a margin comment trailing code), and it's already what
;; the yaml/nodejs/latex extension-dispatching toggles insert for .lisp.
(define (lisp-toggle-comment)
  (let ((row (cursor-row)))
    (move-cursor-beg-line)
    (if (lisp--line-starts-with? (line-text row) ";; ")
        (lisp--delete-n-chars (string-length ";; "))
        (lisp--insert-string ";; " 0))))

;; CTRL-d under "no-prefix" — bound here (not "user") so this package
;; doesn't depend on load order relative to (add-prefix "user") in the
;; master config. The last argument scopes it to this language's files, so
;; every language package can bind its own CTRL-d side by side.
(map-key "CTRL-d" lisp-toggle-comment "no-prefix" (list "lisp" "cl" "scm" "ss" "sld" "el"))

;; ─── Syntax highlighting ────────────────────────────────────────────────────
;; The highlighter tokenizes a "word" as a run of [A-Za-z0-9_#] (plus
;; letters from other scripts), so the '-', '?', '!', '*' that Lisp
;; identifiers are full of end a word. That's why the list below is
;; "define" and not "define-syntax", "set" and not "set!" — the leading
;; alphanumeric part is what actually gets matched, and it still colors
;; the head of the longer name. '#' *is* a word character, though, so
;; "#t" and "#f" match whole.
;;
;; The flip side is that a short keyword also matches a trailing "-word"
;; segment of a hyphenated identifier — "if" lights up inside "my-if".
;; The list keeps the core special forms anyway (highlighting them is the
;; point) but leaves out the ones whose name is *both* a common
;; identifier suffix and not essential: no "block", "return", "values",
;; "error", "assert" ("code-block", "parse-error", "with-values" collide
;; too often). Add them back here if your code doesn't hit that.

(define lisp-keywords
  (list "define" "lambda" "let" "letrec" "if" "cond" "case" "when" "unless"
        "begin" "do" "and" "or" "not" "quote" "quasiquote" "unquote"
        "else" "set" "setq" "setf" "progn" "loop" "dolist" "dotimes"
        "flet" "labels" "catch" "throw" "declare"
        "defun" "defmacro" "defvar" "defparameter" "defconstant" "defstruct"
        "defclass" "defmethod" "defgeneric" "defpackage" "deftype"
        "syntax"))

(define lisp-constants
  (list "#t" "#f" "nil" "true" "false"))

(define (lisp--add-keywords ext words)
  (if (null? words)
      #f
      (begin
        (syntax-add-keyword ext (car words))
        (lisp--add-keywords ext (cdr words)))))

(define (lisp--add-types ext words)
  (if (null? words)
      #f
      (begin
        (syntax-add-type ext (car words))
        (lisp--add-types ext (cdr words)))))

;; ";" line comments, "#| |#" block comments, only '"' opens a string —
;; "'" is quote/quasiquote in Lisp, never a string or char delimiter, so
;; it stays a plain punctuation character. One call per extension, same as
;; the c package registers .c and .h separately.
(define (lisp--register-syntax ext block-open block-close)
  (syntax-define-language ext ";" block-open block-close "\"")
  (lisp--add-keywords ext lisp-keywords)
  (lisp--add-types ext lisp-constants))

;; Common Lisp and Scheme have "#| ... |#" block comments; Emacs Lisp
;; doesn't, so .el registers none rather than mis-reading a stray "#|".
(lisp--register-syntax "lisp" "#|" "|#")
(lisp--register-syntax "cl"   "#|" "|#")
(lisp--register-syntax "scm"  "#|" "|#")
(lisp--register-syntax "ss"   "#|" "|#")
(lisp--register-syntax "sld"  "#|" "|#")
(lisp--register-syntax "el"   ""   "")
