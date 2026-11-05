;; ─── Explorer ───────────────────────────────────────────────────────────────
;; (add-package "explorer") — two features:
;;
;;   CTRL-x e   toggles a persistent file-explorer sidebar (like
;;              VSCode/NERDTree): a narrow left pane listing the project
;;              tree, navigable with the arrow keys. ENTER on a directory
;;              expands/collapses it in place; ENTER on a file opens it
;;              in the main pane, sidebar staying put.
;;
;;   CTRL-x p   dumps a full static tree of the project (like the Unix
;;              `tree` command) into a scratch buffer — for pasting into
;;              a README, sharing, etc. Independent of the sidebar.
;;
;; Built on the generic scratch-buffer/sidebar primitives — editor-pwd,
;; list-directory, editor-scratch-open, editor-scratch-set-lines,
;; editor-explorer-sidebar-open/-close, editor-open-file-path — none of
;; this is hardcoded "file explorer" logic in C, just building blocks any
;; package could reuse for its own generated-content sidebar or buffer.
;;
;; Dotfiles (names starting with ".") are skipped in both features — the
;; overwhelming majority of the time nobody wants .git's internals in a
;; tree view. Fork this file if you want them.

;; ─── shared helpers ───────────────────────────────────────────────────────

(define (explorer--hidden? name)
  (char=? (string-ref name 0) #\.))

(define (explorer--entry<? a b)
  ;; Directories before files; alphabetical within each group.
  (let ((a-dir (cdr a)) (b-dir (cdr b)))
    (if (eq? a-dir b-dir)
        (string<? (car a) (car b))
        a-dir)))

(define (explorer--visible-entries path)
  (sort (filter (lambda (entry) (not (explorer--hidden? (car entry))))
                (list-directory path))
        explorer--entry<?))

(define (explorer--last-slash-index s i best)
  (cond ((>= i (string-length s)) best)
        ((char=? (string-ref s i) #\/) (explorer--last-slash-index s (+ i 1) i))
        (else (explorer--last-slash-index s (+ i 1) best))))

(define (explorer--basename s)
  (substring s (+ (explorer--last-slash-index s 0 -1) 1) (string-length s)))

(define (explorer--remove x lst)
  (cond ((null? lst) '())
        ((string=? (car lst) x) (cdr lst))
        (else (cons (car lst) (explorer--remove x (cdr lst))))))

(define (explorer--nth lst n)
  (if (= n 0) (car lst) (explorer--nth (cdr lst) (- n 1))))

(define (explorer--row-of path paths idx)
  (cond ((null? paths) -1)
        ((string=? (car (car paths)) path) idx)
        (else (explorer--row-of path (cdr paths) (+ idx 1)))))

;; ─── interactive sidebar ────────────────────────────────────────────────────

(define explorer-buffer-id -1)
(define explorer-open? #f)
(define explorer-root "")
(define explorer-expanded '())   ; list of expanded directory paths
(define explorer-lines '())      ; current display lines (parallel to -paths)
(define explorer-paths '())      ; list of (path . is-dir), same order as -lines
(define explorer-sidebar-width 30)

(define (explorer--indent depth)
  (if (<= depth 0) "" (string-append "  " (explorer--indent (- depth 1)))))

(define (explorer--make-line depth name is-dir expanded)
  (string-append (explorer--indent depth)
                 (cond ((not is-dir) "  ")
                       (expanded "▾ ")
                       (else "▸ "))
                 name
                 (if is-dir "/" "")))

(define (explorer--visit path name depth is-dir)
  (let ((expanded (and is-dir (member path explorer-expanded))))
    (push (explorer--make-line depth name is-dir expanded) explorer-lines)
    (push (cons path is-dir) explorer-paths)
    (if expanded
        (explorer--visit-entries (explorer--visible-entries path) path (+ depth 1))
        '())))

(define (explorer--visit-entries entries dir-path depth)
  (if (null? entries)
      '()
      (begin
        (let* ((entry (car entries))
               (name (car entry))
               (is-dir (cdr entry))
               (child-path (string-append dir-path "/" name)))
          (explorer--visit child-path name depth is-dir))
        (explorer--visit-entries (cdr entries) dir-path depth))))

(define (explorer-refresh)
  ;; editor-scratch-set-lines resets the cursor to row 0 as part of
  ;; replacing the buffer's content — without restoring it afterward,
  ;; expanding/collapsing a directory would bounce you back to the top
  ;; of the tree every single time instead of staying where you were.
  (let ((selected-path (if (null? explorer-paths)
                            explorer-root
                            (car (explorer--nth explorer-paths (cursor-row))))))
    (set! explorer-lines '())
    (set! explorer-paths '())
    (explorer--visit explorer-root (explorer--basename explorer-root) 0 #t)
    (set! explorer-lines (reverse explorer-lines))
    (set! explorer-paths (reverse explorer-paths))
    (editor-scratch-set-lines explorer-buffer-id explorer-lines)
    (let ((restored-row (explorer--row-of selected-path explorer-paths 0)))
      (editor-goto-line (if (>= restored-row 0) restored-row 0)))))

;; Roots the tree at the current project (project-root) — so opening
;; the sidebar from a subdirectory still shows the whole project, not just
;; wherever the editor happened to be started from. Outside any project,
;; it asks for a directory instead of silently defaulting to the working
;; directory (which might not be what you meant to browse); ESC there
;; leaves the sidebar closed, the same as declining the LSP-start prompt.
(define (explorer-open)
  (if explorer-open?
      '()
      (let* ((git-root (project-root))
             (root (if (string=? git-root "")
                       (editor-prompt "Explorer root: %u (ESC to cancel)"
                                      (editor-pwd) #t)
                       git-root)))
        (if root
            (begin
              (set! explorer-root root)
              (set! explorer-expanded (list explorer-root))
              (set! explorer-buffer-id (editor-explorer-sidebar-open "*explorer*" explorer-sidebar-width))
              (set! explorer-open? #t)
              (explorer-refresh))
            '()))))

(define (explorer-close)
  (if explorer-open?
      (begin
        (editor-explorer-close)
        (set! explorer-open? #f))
      '()))

(define (explorer-toggle)
  (if explorer-open? (explorer-close) (explorer-open)))

;; Killing the sidebar's buffer (CTRL-x k) already collapses the split; just
;; forget it here, or explorer-open? stays #t and the next toggle would
;; "close" a sidebar that's already gone instead of opening a new one.
(add-hook "buffer-killed"
          (lambda (name id)
            (if (= id explorer-buffer-id)
                (begin
                  (set! explorer-open? #f)
                  (set! explorer-buffer-id -1))
                '())))

(define (explorer-activate)
  (if (null? explorer-paths)
      '()
      (let* ((entry (explorer--nth explorer-paths (cursor-row)))
             (path (car entry))
             (is-dir (cdr entry)))
        (if is-dir
            (begin
              (if (member path explorer-expanded)
                  (set! explorer-expanded (explorer--remove path explorer-expanded))
                  (push path explorer-expanded))
              (explorer-refresh))
            (begin
              (editor-switch-next-pane)
              (editor-open-file-path path))))))

;; Sticky (see add-prefix): arrow-key navigation shouldn't kick you back
;; to "no-prefix" after every single keystroke — only opening a file or
;; explicitly closing the sidebar should leave "explorer" mode.
(add-prefix "explorer" #t)
(map-key "ENTER" explorer-activate "explorer")
(map-key "ARROW-UP" '(move-cursor "up") "explorer")
(map-key "ARROW-DOWN" '(move-cursor "down") "explorer")
(map-key "q" explorer-close "explorer")
(map-key "e" explorer-toggle "user")
(map-key "ESCAPE" '(editor-set-prefix "no-prefix") "explorer")
(map-key "CTRL-x" '(editor-set-prefix "user") "explorer")

;; ─── static tree dump ───────────────────────────────────────────────────────

(define (tree--connector is-last)
  (if is-last "└── " "├── "))

(define (tree--child-prefix prefix is-last)
  (string-append prefix (if is-last "    " "│   ")))

(define (tree--build path prefix)
  (tree--build-entries (explorer--visible-entries path) path prefix))

(define (tree--build-entries entries dir-path prefix)
  (if (null? entries)
      '()
      (let* ((entry (car entries))
             (name (car entry))
             (is-dir (cdr entry))
             (is-last (null? (cdr entries)))
             (line (string-append prefix (tree--connector is-last) name (if is-dir "/" "")))
             (child-lines (if is-dir
                               (tree--build (string-append dir-path "/" name)
                                            (tree--child-prefix prefix is-last))
                               '())))
        (cons line (append child-lines (tree--build-entries (cdr entries) dir-path prefix))))))

(define (project-tree)
  (let* ((project (project-root))
         (root (if (string=? project "") (editor-pwd) project))
         (lines (cons (string-append root "/") (tree--build root "")))
         (buf-id (editor-scratch-open "*tree*")))
    (editor-scratch-set-lines buf-id lines)
    (editor-switch-to-buffer buf-id)))

(map-key "CTRL-p" project-tree "user")
