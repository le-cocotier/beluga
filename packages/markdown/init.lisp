;; ─── Markdown ───────────────────────────────────────────────────────────────
;; (add-package "markdown") — highlighting, marksman as the language server,
;; a heading outline on CTRL-o and list continuation on ENTER, for .md and
;; .markdown files.
;;
;; marksman is optional: without it you still get everything below except
;; what the server adds (completion of links and headings, go to definition
;; on a [link](other.md#heading), diagnostics for broken links). It's a
;; single binary:
;;   `brew install marksman`, or https://github.com/artempyanykh/marksman/releases

(lsp-server "md" "markdown" "marksman" "server")
(lsp-server "markdown" "markdown" "marksman" "server")

(define markdown--extensions (list "md" "markdown"))

;; ─── Syntax highlighting ────────────────────────────────────────────────────
;; Built from the generic primitives, not a Markdown parser:
;;
;;   - ``` fenced code blocks are registered as the language's block comment,
;;     ``` opening and ``` closing it. That reuses the cross-row comment-state
;;     cache, so a fence opened above the top of the pane is still drawn as
;;     code after a scroll, and a "# comment" inside one isn't a heading.
;;     ~~~ fences aren't recognized (one delimiter pair per language).
;;   - `inline code` is the only quote character, so it's drawn as a string.
;;     Not ' or ", which are just apostrophes and quotation marks in prose.
;;   - Whole-line rules for headings, quotes, rules and setext underlines.
;;     List lines are left alone: coloring a whole bullet would color most
;;     of a typical README.

(define (markdown--register-syntax ext)
  (syntax-define-language ext "" "```" "```" "`")
  (syntax-add-line-prefix ext "#" "keyword")
  (syntax-add-line-prefix ext "===" "keyword")
  (syntax-add-line-prefix ext ">" "comment")
  (syntax-add-line-prefix ext "---" "comment")
  (syntax-add-line-prefix ext "***" "comment"))

(markdown--register-syntax "md")
(markdown--register-syntax "markdown")

;; ─── Helpers ────────────────────────────────────────────────────────────────

(define (markdown--space? c)
  (or (char=? c #\space) (char=? c #\tab)))

;; Index of the first character at or after `i` that isn't a space or tab.
(define (markdown--skip-spaces s i)
  (if (and (< i (string-length s)) (markdown--space? (string-ref s i)))
      (markdown--skip-spaces s (+ i 1))
      i))

(define (markdown--skip-digits s i)
  (if (and (< i (string-length s)) (char-numeric? (string-ref s i)))
      (markdown--skip-digits s (+ i 1))
      i))

(define (markdown--char-at? s i c)
  (and (< i (string-length s)) (char=? (string-ref s i) c)))

(define (markdown--starts-at? s i prefix)
  (let ((end (+ i (string-length prefix))))
    (and (<= end (string-length s)) (string=? (substring s i end) prefix))))

(define (markdown--fence? line)
  (markdown--starts-at? line (markdown--skip-spaces line 0) "```"))

;; Whether `row` sits inside a ``` block: an odd number of fence lines above
;; it. Rescans from the top, which is fine at the size Markdown files run to.
(define (markdown--fence-state r row inside)
  (if (>= r row)
      inside
      (markdown--fence-state (+ r 1) row
                             (if (markdown--fence? (line-text r)) (not inside) inside))))

(define (markdown--in-fence? row)
  (markdown--fence-state 0 row #f))

;; ─── List continuation ──────────────────────────────────────────────────────
;; ENTER on a list item or a quote starts the next one:
;;
;;   - item        ->  "- "         (also * and +)
;;   - [x] task    ->  "- [ ] "
;;   3. step       ->  "4. "        (also "3)")
;;   > quote       ->  "> "         (nested "> > " too)
;;
;; with the item's indentation kept. ENTER on an item with nothing after
;; the marker removes the marker instead, which is how you end a list.
;; Anywhere else, inside a ``` block, or with the cursor still inside the
;; marker, it's a plain newline.

;; For a line that opens with a marker: (length-of-marker next-marker),
;; where the length includes the indentation. #f otherwise.
(define (markdown--list-marker line)
  (let* ((k (markdown--skip-spaces line 0))
         (indent (substring line 0 k)))
    (cond ((markdown--char-at? line k #\>)
           (let ((end (markdown--quote-end line k)))
             (list end (substring line 0 end))))
          ((and (or (markdown--char-at? line k #\-)
                    (markdown--char-at? line k #\*)
                    (markdown--char-at? line k #\+))
                (markdown--char-at? line (+ k 1) #\space))
           (let ((bullet (substring line 0 (+ k 2))))
             (if (or (markdown--starts-at? line (+ k 2) "[ ] ")
                     (markdown--starts-at? line (+ k 2) "[x] ")
                     (markdown--starts-at? line (+ k 2) "[X] "))
                 (list (+ k 6) (string-append bullet "[ ] "))
                 (list (+ k 2) bullet))))
          (else
           (let ((d (markdown--skip-digits line k)))
             (if (and (> d k)
                      (or (markdown--char-at? line d #\.) (markdown--char-at? line d #\)))
                      (markdown--char-at? line (+ d 1) #\space))
                 (list (+ d 2)
                       (string-append indent
                                      (number->string (+ (string->number (substring line k d)) 1))
                                      (substring line d (+ d 2))))
                 #f))))))

;; End of a run of ">" markers starting at `i`, each with its optional
;; space: "> > text" -> the index of "t".
(define (markdown--quote-end line i)
  (if (markdown--char-at? line i #\>)
      (markdown--quote-end line (markdown--skip-spaces line (+ i 1)))
      i))

(define (markdown--blank-from? line i)
  (>= (markdown--skip-spaces line i) (string-length line)))

(define (markdown-newline)
  (let* ((row (cursor-row))
         (line (line-text row))
         (marker (if (markdown--in-fence? row) #f (markdown--list-marker line))))
    (cond ((or (not marker) (< (cursor-col) (car marker)))
           (editor-insert-new-line))
          ;; Nothing but the marker (and blanks): plain ASCII, so the
          ;; string length is also the column count.
          ((markdown--blank-from? line (car marker))
           (buffer-delete-range row 0 row (string-length line)))
          (else
           (begin
             (editor-insert-new-line)
             (buffer-insert-string (car (cdr marker))))))))

(map-key "ENTER" markdown-newline "no-prefix" markdown--extensions)

;; ─── Heading outline ────────────────────────────────────────────────────────
;; CTRL-o in a Markdown file lists its headings instead of the outline
;; package's brace scan, which finds nothing in prose. marksman's symbols
;; win when it's running, the same as clangd's do for the outline package;
;; without it, the list comes from the "#" lines outside ``` blocks,
;; indented by level.

(define (markdown--heading-level line i)
  (if (markdown--char-at? line i #\#) (markdown--heading-level line (+ i 1)) i))

(define (markdown--heading? line)
  (let ((level (markdown--heading-level line 0)))
    (and (> level 0)
         (<= level 6)
         (or (= level (string-length line)) (markdown--space? (string-ref line level))))))

(define (markdown--indent n)
  (if (<= n 0) "" (string-append "  " (markdown--indent (- n 1)))))

(define (markdown--heading-entry line row)
  (let ((level (markdown--heading-level line 0)))
    (cons (string-append (number->string (+ row 1)) ": "
                         (markdown--indent (- level 1))
                         (substring line (markdown--skip-spaces line level) (string-length line)))
          row)))

(define (markdown--headings row in-fence acc)
  (if (>= row (buffer-line-count))
      (reverse acc)
      (let ((line (line-text row)))
        (cond ((markdown--fence? line)
               (markdown--headings (+ row 1) (not in-fence) acc))
              ((and (not in-fence) (markdown--heading? line))
               (markdown--headings (+ row 1) in-fence (cons (markdown--heading-entry line row) acc)))
              (else (markdown--headings (+ row 1) in-fence acc))))))

(define (markdown-outline)
  (let ((symbols (lsp-document-symbols)))
    (editor-list-popup "Headings"
                       (if (null? symbols) (markdown--headings 0 #f '()) symbols)
                       (lambda (row)
                         (begin
                           (editor-goto-line row)
                           (editor-recenter)))
                       '()
                       #t)))

(map-key "CTRL-o" markdown-outline "no-prefix" markdown--extensions)
