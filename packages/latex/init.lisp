;; ─── LaTeX ──────────────────────────────────────────────────────────────────
;; (add-package "latex") — registers a LaTeX language server for .tex/.sty/
;; .cls/.bib, a CTRL-d comment toggle, and CTRL-x l to insert a \begin/\end
;; environment picked from a list.
;;
;; Uses digestif by default because it ships with TeX Live and MacTeX, so
;; there's usually nothing to install: it's already on PATH next to
;; pdflatex. texlab is the more capable server (better cross-file
;; navigation and diagnostics) if you'd rather install it — swap the lines
;; below. Both speak LSP over stdio and take no arguments.
;;
;;   texlab: `brew install texlab`, `cargo install texlab`, or
;;           https://github.com/latex-lsp/texlab/releases

(lsp-server "tex" "latex" "digestif")
(lsp-server "sty" "latex" "digestif")
(lsp-server "cls" "latex" "digestif")
(lsp-server "bib" "bibtex" "digestif")

;; (lsp-server "tex" "latex" "texlab")
;; (lsp-server "sty" "latex" "texlab")
;; (lsp-server "cls" "latex" "texlab")
;; (lsp-server "bib" "bibtex" "texlab")

;; ─── Comment toggle ─────────────────────────────────────────────────────────
;; Unlike the c/python/rust packages, whose toggles each hardcode one
;; comment prefix, this one looks at the file's extension and picks the
;; right prefix. Those packages all bind CTRL-d under "no-prefix" and the
;; last binding registered wins, so with several language packages loaded
;; exactly one toggle is ever reachable — and a hardcoded one then happily
;; inserts "// " into a .tex file. Dispatching on the extension means it
;; doesn't matter which package won the key.
;;
;; Load this package AFTER c/python/rust in your init.lisp if you want
;; that behavior; load it before and their (correct, but single-language)
;; toggle takes the key instead.

(define latex--comment-styles
  (list (cons "tex" "% ") (cons "sty" "% ") (cons "cls" "% ") (cons "bib" "% ")
        (cons "c" "// ") (cons "h" "// ") (cons "cpp" "// ") (cons "hpp" "// ")
        (cons "js" "// ") (cons "jsx" "// ") (cons "mjs" "// ") (cons "cjs" "// ")
        (cons "ts" "// ") (cons "tsx" "// ") (cons "rs" "// ")
        (cons "py" "# ") (cons "yaml" "# ") (cons "yml" "# ")
        (cons "toml" "# ") (cons "sh" "# ") (cons "lisp" ";; ")))

(define (latex--lookup key styles)
  (if (null? styles)
      ""
      (if (string=? key (car (car styles)))
          (cdr (car styles))
          (latex--lookup key (cdr styles)))))

;; Index of the last `ch` in `s`, or -1. Plain recursion, like the other
;; packages — the interpreter has no string-search primitive.
(define (latex--last-index-of s ch i best)
  (if (< i (string-length s))
      (latex--last-index-of s ch (+ i 1)
                            (if (char=? (string-ref s i) ch) i best))
      best))

;; The current buffer's extension, lowercased, or "" for a path with no
;; extension at all. The dot has to come after the last slash, or a file
;; called "notes" inside "~/v1.2/" would come out with the extension "2/notes".
(define (latex--extension)
  (let* ((path (buffer-filename))
         (dot (latex--last-index-of path #\. 0 -1))
         (slash (latex--last-index-of path #\/ 0 -1)))
    (if (and (>= dot 0) (> dot slash))
        (string-downcase (substring path (+ dot 1) (string-length path)))
        "")))

(define (latex--line-starts-with? s prefix)
  (let ((s-len (string-length s)) (p-len (string-length prefix)))
    (and (>= s-len p-len) (string=? (substring s 0 p-len) prefix))))

(define (latex--insert-string s i)
  (if (< i (string-length s))
      (begin
        (editor-insert-char (string (string-ref s i)))
        (latex--insert-string s (+ i 1)))))

;; Plain recursion rather than (dotimes ...) — dotimes' body currently
;; only reliably calls raw C builtins, not user-defined Lisp lambdas like
;; editor-delete-next-char, so it silently stops after the first
;; iteration here.
(define (latex--delete-n-chars n)
  (if (< 0 n)
      (begin
        (editor-delete-next-char)
        (latex--delete-n-chars (- n 1)))))

;; Comments and uncomments at column 0, same as the c/python/rust toggles.
;; An indented line gets its marker at the far left rather than in front of
;; the text — valid everywhere this is bound, if not pretty.
(define (latex-toggle-comment)
  (let* ((prefix (latex--lookup (latex--extension) latex--comment-styles))
         (row (cursor-row)))
    (if (string=? prefix "")
        #f                      ; unknown file type: leave the line alone
        (begin
          (move-cursor-beg-line)
          (if (latex--line-starts-with? (line-text row) prefix)
              (latex--delete-n-chars (string-length prefix))
              (latex--insert-string prefix 0))))))

;; ─── Environments ───────────────────────────────────────────────────────────
;; CTRL-x l pops up a list of the environments you actually reach for, and
;; inserts the \begin/\end pair with an indented blank line between them,
;; cursor on it. Built on (editor-list-popup ...), so it's themed like
;; every other popup and needs no C.

(define latex-environments
  (list (cons "itemize"    "itemize")
        (cons "enumerate"  "enumerate")
        (cons "description" "description")
        (cons "figure"     "figure")
        (cons "table"      "table")
        (cons "tabular"    "tabular")
        (cons "equation"   "equation")
        (cons "align"      "align")
        (cons "center"     "center")
        (cons "quote"      "quote")
        (cons "verbatim"   "verbatim")
        (cons "frame"      "frame")))

(define (latex-insert-environment)
  (editor-list-popup "LaTeX environment"
                     latex-environments
                     (lambda (name)
                       (begin
                         ;; From column 0, so invoking this mid-line pushes the
                         ;; whole line down instead of splitting it in half.
                         (move-cursor-beg-line)
                         (latex--insert-string (string-append "\\begin{" name "}") 0)
                         (editor-insert-new-line)
                         (editor-insert-new-line)
                         (latex--insert-string (string-append "\\end{" name "}") 0)
                         ;; One more break, or whatever was on this line stays
                         ;; stuck to the end of the \\end{...}.
                         (editor-insert-new-line)
                         ;; back onto the blank line between begin and end
                         (move-cursor "up")
                         (move-cursor "up")
                         (editor-insert-tab)))))

;; CTRL-d under "no-prefix" — bound here (not "user") so this package
;; doesn't depend on load order relative to (add-prefix "user") in the
;; master config.
(map-key "CTRL-d" latex-toggle-comment "no-prefix")
(map-key "l" latex-insert-environment "user")

;; ─── Syntax highlighting ────────────────────────────────────────────────────
;; The highlighter's word matcher scans runs of identifier characters
;; wherever they occur, independent of what comes before them — so
;; registering "section" as a keyword lights up the word inside
;; "\section{...}" too, even though the backslash itself isn't part of the
;; match. That's the same trick that makes "#include" work as a whole
;; C keyword despite the '#'.

(define latex-keywords
  (list "documentclass" "usepackage" "begin" "end" "section" "subsection"
        "subsubsection" "chapter" "part" "paragraph" "item" "label" "ref"
        "cite" "footnote" "caption" "title" "author" "date" "maketitle"
        "tableofcontents" "newcommand" "renewcommand" "input" "include"
        "bibliography" "bibliographystyle"))

(define (latex--add-keywords ext words)
  (if (null? words)
      #f
      (begin
        (syntax-add-keyword ext (car words))
        (latex--add-keywords ext (cdr words)))))

;; "%" line comments, no block comment (LaTeX doesn't have one — \iffalse
;; ... \fi is a conditional, not a comment delimiter the highlighter can
;; model generically), no quote characters (LaTeX's `` ... '' convention
;; doesn't map onto a single open/close char the way "..." does elsewhere).
(define (latex--register-syntax ext)
  (syntax-define-language ext "%" "" "" "")
  (latex--add-keywords ext latex-keywords))

(latex--register-syntax "tex")
(latex--register-syntax "sty")
(latex--register-syntax "cls")

;; ─── BibTeX ─────────────────────────────────────────────────────────────────
;; .bib is a different (if related) syntax — entry types and field names
;; rather than control sequences, and real quoted field values ("...").

(define bib-keywords
  (list "article" "book" "inproceedings" "incollection" "phdthesis"
        "mastersthesis" "techreport" "misc" "unpublished" "proceedings"
        "manual" "booklet" "author" "title" "journal" "year" "volume"
        "number" "pages" "publisher" "editor" "note" "url" "doi" "isbn"
        "month" "address" "organization" "school" "institution"))

(define (bib--add-keywords words)
  (if (null? words)
      #f
      (begin
        (syntax-add-keyword "bib" (car words))
        (bib--add-keywords (cdr words)))))

(syntax-define-language "bib" "%" "" "" "\"")
(bib--add-keywords bib-keywords)
