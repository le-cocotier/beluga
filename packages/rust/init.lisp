;; ─── Rust ───────────────────────────────────────────────────────────────────
;; (add-package "rust") — registers rust-analyzer for .rs files, plus a
;; CTRL-d line-comment toggle so the package demonstrates more than just
;; LSP wiring. Fork/extend with whatever else your Rust workflow needs.
;;
;; Needs rust-analyzer on PATH: https://rust-analyzer.github.io/ (or
;; `rustup component add rust-analyzer`).

(lsp-server "rs" "rust" "rust-analyzer")

(define (rust--line-starts-with? s prefix)
  (let ((s-len (string-length s)) (p-len (string-length prefix)))
    (and (>= s-len p-len) (string=? (substring s 0 p-len) prefix))))

(define (rust--insert-string s i)
  (if (< i (string-length s))
      (begin
        (editor-insert-char (string (string-ref s i)))
        (rust--insert-string s (+ i 1)))))

;; Plain recursion rather than (dotimes ...) — dotimes' body currently
;; only reliably calls raw C builtins, not user-defined Lisp lambdas like
;; editor-delete-next-char, so it silently stops after the first
;; iteration here.
(define (rust--delete-n-chars n)
  (if (< 0 n)
      (begin
        (editor-delete-next-char)
        (rust--delete-n-chars (- n 1)))))

(define (rust-toggle-comment)
  (let ((row (cursor-row)))
    (move-cursor-beg-line)
    (if (rust--line-starts-with? (line-text row) "// ")
        (rust--delete-n-chars (string-length "// "))
        (rust--insert-string "// " 0))))

;; CTRL-d under "no-prefix" — bound here (not "user") so this package
;; doesn't depend on load order relative to (add-prefix "user") in the
;; master config. The last argument scopes it to this language's files, so
;; every language package can bind its own CTRL-d side by side.
(map-key "CTRL-d" rust-toggle-comment "no-prefix" "rs")

;; ─── Syntax highlighting ────────────────────────────────────────────────────

(define rust-keywords
  (list "fn" "let" "mut" "if" "else" "while" "loop" "match" "return" "break"
        "continue" "struct" "enum" "impl" "trait" "pub" "use" "mod" "crate"
        "self" "Self" "super" "as" "where" "dyn" "move" "async" "await"
        "unsafe" "extern" "static" "const" "type" "ref" "in" "for"))

(define rust-types
  (list "i8" "i16" "i32" "i64" "i128" "isize" "u8" "u16" "u32" "u64" "u128"
        "usize" "f32" "f64" "bool" "char" "str" "String" "Vec" "Option"
        "Some" "None" "Result" "Ok" "Err"))

(define (rust--add-keywords words)
  (if (null? words)
      #f
      (begin
        (syntax-add-keyword "rs" (car words))
        (rust--add-keywords (cdr words)))))

(define (rust--add-types words)
  (if (null? words)
      #f
      (begin
        (syntax-add-type "rs" (car words))
        (rust--add-types (cdr words)))))

;; "//" line comments, "/* */" block comments (Rust's block comments nest,
;; which this generic highlighter doesn't model — a nested /* */ pair
;; closes highlighting early, same simplification every other language
;; here makes), both quote styles open a string/char literal.
(syntax-define-language "rs" "//" "/*" "*/" "\"'")
(rust--add-keywords rust-keywords)
(rust--add-types rust-types)
