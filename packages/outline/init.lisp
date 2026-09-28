;; ─── Outline ────────────────────────────────────────────────────────────────
;; (add-package "outline") — CTRL-o opens a popup listing every top-level
;; definition in the current buffer; ENTER jumps the cursor to it, ESC
;; dismisses without moving anything.
;;
;; Built entirely on the generic introspection/popup primitives exposed to
;; Lisp — (line-text n), (buffer-line-count), (editor-list-popup ...),
;; (editor-goto-line row) — none of this is hardcoded in C. Any package can
;; reach for the same handful of functions to build its own picker: a TODO
;; scanner, a git-status list, a fuzzy file finder, and so on.
;;
;; The scan itself is a simple heuristic, not a real parser: a line counts
;; as a definition if it doesn't start with leading whitespace and, once
;; trailing whitespace is stripped, ends in "{". That catches top-level
;; function bodies and struct/enum blocks in most C-like languages. Good
;; enough as a first pass — a language server integration could build the
;; exact same (label . line) list from LSP's textDocument/documentSymbol
;; instead and hand it to this same (editor-list-popup ...) call.

(define (outline--blank-or-space? c)
  (or (char=? c #\space) (char=? c #\tab) (char=? c #\return)))

(define (outline--last-non-space s i)
  (if (or (< i 0) (not (outline--blank-or-space? (string-ref s i))))
      i
      (outline--last-non-space s (- i 1))))

(define (outline--rtrim s)
  (let ((end (outline--last-non-space s (- (string-length s) 1))))
    (if (< end 0) "" (substring s 0 (+ end 1)))))

(define (outline--indented? line)
  (and (> (string-length line) 0)
       (outline--blank-or-space? (string-ref line 0))))

(define (outline--definition? line)
  (let ((trimmed (outline--rtrim line)))
    (and (> (string-length trimmed) 0)
         (not (outline--indented? line))
         (char=? (string-ref trimmed (- (string-length trimmed) 1)) #\{))))

;; Walks every row of the current buffer, collecting one (label . row) pair
;; per definition found, in file order.
(define (outline--entries row acc)
  (if (>= row (buffer-line-count))
      (reverse acc)
      (let ((text (line-text row)))
        (outline--entries
         (+ row 1)
         (if (outline--definition? text)
             (cons (cons (string-append (number->string row) ": " (outline--rtrim text)) row)
                   acc)
             acc)))))

;; #t at the end asks for a live-filtering popup — type to narrow the list
;; by substring, same as CTRL-P's file finder. See Builtins.md /
;; editor-list-popup for what the 5th argument does.
(define (editor-outline)
  (editor-list-popup "Outline"
                      (outline--entries 0 '())
                      ;; Jump, then scroll so the definition lands mid-pane
                      ;; with context around it (not on the bottom row).
                      (lambda (row)
                        (begin
                          (editor-goto-line row)
                          (editor-recenter)))
                      '()
                      #t))

(map-key "CTRL-o" editor-outline "no-prefix")
