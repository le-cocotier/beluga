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
;; master config. If you also load another language package that binds
;; CTRL-d (c, python, ...), whichever loads first wins; the other's
;; binding is silently unreachable — rebind one if you use both together.
(map-key "CTRL-d" rust-toggle-comment "no-prefix")
