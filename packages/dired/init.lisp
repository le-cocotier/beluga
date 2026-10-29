;; ─── Dired ──────────────────────────────────────────────────────────────────
;; (add-package "dired") — a directory editor modelled on Emacs' dired.
;;
;;   CTRL-x d       prompt for a directory and list it
;;   CTRL-x CTRL-j  list the current file's directory, cursor on the file
;;
;; Inside the *dired* buffer (sticky "dired" prefix):
;;
;;   n / p, arrows, SPACE   move          ENTER / f / e  open file or directory
;;   ^ / BACKSPACE          parent dir    g              re-read the directory
;;   m / u                  mark / unmark U              unmark everything
;;   t                      toggle marks  d              flag for deletion
;;   x                      delete flagged                 D  delete marked
;;   R                      rename/move   C              copy
;;   +                      new directory !              shell command on files
;;   s                      cycle sort (name, date, size)
;;   .                      show/hide dotfiles
;;   ?                      key summary   q              quit (kill the buffer)
;;
;; D, R, C and ! act on the marked files, or on the file under the cursor
;; when nothing is marked — the same rule as Emacs.
;;
;; Settings (override them after the add-package line, e.g. in
;; personal-config):
(define dired-show-hidden #f)     ; list dotfiles?
(define dired-sort-mode "name")   ; "name" (directories first), "date", "size"

;; Built on: list-directory, file-attributes, delete-path, move-paths,
;; copy-paths, make-directories (the editor's own file operations — the
;; same on every platform, no rm/mv/cp/mkdir involved), shell-run (for !),
;; expand-path, editor-prompt, editor-message, and the scratch-buffer
;; primitives.
;;
;; Every operation runs synchronously: copying a large tree blocks the
;; editor until it's done.

;; ─── state ──────────────────────────────────────────────────────────────────

(define dired-buffer-id -1)
(define dired-dir "")
;; One vector per listed entry:
;;   #(name is-dir is-link size mtime mode date link-target mark)
;; mark is " " (none), "*" (marked) or "D" (flagged for deletion).
(define dired-entries (list->vector '()))
(define dired--header-rows 3)   ; path, summary, blank line
(define dired--name-column 34)  ; where the file name starts on each row

(define (dired--name e)    (vector-ref e 0))
(define (dired--dir? e)    (vector-ref e 1))
(define (dired--link? e)   (vector-ref e 2))
(define (dired--size e)    (vector-ref e 3))
(define (dired--mtime e)   (vector-ref e 4))
(define (dired--mode e)    (vector-ref e 5))
(define (dired--date e)    (vector-ref e 6))
(define (dired--target e)  (vector-ref e 7))
(define (dired--mark e)    (vector-ref e 8))
(define (dired--parent-entry? e) (string=? (dired--name e) ".."))

;; ─── string & path helpers ──────────────────────────────────────────────────

(define (dired--join dir name)
  (if (string=? dir "/") (string-append "/" name) (string-append dir "/" name)))

(define (dired--path e) (dired--join dired-dir (dired--name e)))

(define (dired--dir-slash)
  (if (string=? dired-dir "/") "/" (string-append dired-dir "/")))

(define (dired--last-slash s i best)
  (cond ((>= i (string-length s)) best)
        ((char=? (string-ref s i) #\/) (dired--last-slash s (+ i 1) i))
        (else (dired--last-slash s (+ i 1) best))))

(define (dired--basename s)
  (substring s (+ (dired--last-slash s 0 -1) 1) (string-length s)))

(define (dired--dirname s) (expand-path ".." s))

(define (dired--pad-left s width)
  (if (< (string-length s) width)
      (string-append (make-string (- width (string-length s)) #\space) s)
      s))

;; Prompts are printf-style format strings: a "%" in a file name would be
;; read as a format directive. Swap it for the look-alike FULLWIDTH PERCENT.
(define (dired--replace-percent s i start acc)
  (cond ((>= i (string-length s)) (string-append acc (substring s start i)))
        ((char=? (string-ref s i) #\%)
         (dired--replace-percent s (+ i 1) (+ i 1)
                                 (string-append acc (substring s start i) "％")))
        (else (dired--replace-percent s (+ i 1) start acc))))

(define (dired--prompt-safe s) (dired--replace-percent s 0 0 ""))

;; First line of s, at most 200 characters — for status-bar messages.
(define (dired--newline-index s i limit)
  (cond ((>= i limit) limit)
        ((char=? (string-ref s i) #\newline) i)
        (else (dired--newline-index s (+ i 1) limit))))

(define (dired--first-line s)
  (substring s 0 (dired--newline-index s 0 (min (string-length s) 200))))

;; 1234 -> "1234", 45678 -> "44.6K", 7340032 -> "7.0M" (ls -h style).
(define (dired--human-units n units)
  (if (or (< n (* 1024 1024)) (null? (cdr units)))
      (let ((tenths (quotient (* n 10) 1024)))
        (if (< tenths 100)
            (string-append (number->string (quotient tenths 10)) "."
                           (number->string (remainder tenths 10)) (car units))
            (string-append (number->string (quotient n 1024)) (car units))))
      (dired--human-units (quotient n 1024) (cdr units))))

(define (dired--human n)
  (if (< n 1024) (number->string n) (dired--human-units n (list "K" "M" "G" "T" "P"))))

(define (dired--plural n singular plural)
  (string-append (number->string n) " " (if (= n 1) singular plural)))

(define (dired--describe entries)
  (if (null? (cdr entries))
      (dired--name (car entries))
      (dired--plural (length entries) "file" "files")))

(define (dired--confirm? question)
  (let ((answer (editor-prompt (string-append (dired--prompt-safe question)
                                              " (yes or no) %u")
                               "" #f)))
    (and answer (string=? answer "yes"))))

;; ─── reading a directory ────────────────────────────────────────────────────

(define (dired--make-entry dir name)
  (let ((attrs (file-attributes (dired--join dir name))))
    (if attrs
        (list->vector (cons name (append attrs (list " "))))
        #f)))

(define (dired--hidden? name) (char=? (string-ref name 0) #\.))

(define (dired--by-name a b)
  (if (eq? (dired--dir? a) (dired--dir? b))
      (string<? (dired--name a) (dired--name b))
      (dired--dir? a)))

(define (dired--comparator)
  (cond ((string=? dired-sort-mode "date") (lambda (a b) (> (dired--mtime a) (dired--mtime b))))
        ((string=? dired-sort-mode "size") (lambda (a b) (> (dired--size a) (dired--size b))))
        (else dired--by-name)))

(define (dired--read dir)
  (let* ((names (map1 car (list-directory dir)))
         (shown (if dired-show-hidden
                    names
                    (filter (lambda (n) (not (dired--hidden? n))) names)))
         (entries (filter (lambda (e) e)
                          (map1 (lambda (n) (dired--make-entry dir n)) shown)))
         (sorted (sort entries (dired--comparator)))
         (up (if (string=? dir "/") #f (dired--make-entry dir ".."))))
    (list->vector (if up (cons up sorted) sorted))))

;; ─── walking the entry vector ───────────────────────────────────────────────

(define (dired--each proc i)
  (if (< i (vector-length dired-entries))
      (begin
        (proc (vector-ref dired-entries i))
        (dired--each proc (+ i 1)))
      '()))

(define (dired--for-each-file proc)
  (dired--each (lambda (e) (if (dired--parent-entry? e) '() (proc e))) 0))

(define (dired--index-of name i)
  (cond ((>= i (vector-length dired-entries)) -1)
        ((string=? (dired--name (vector-ref dired-entries i)) name) i)
        (else (dired--index-of name (+ i 1)))))

(define (dired--with-mark m)
  (filter (lambda (e) (string=? (dired--mark e) m)) (vector->list dired-entries)))

(define (dired--file-count)
  (- (vector-length dired-entries)
     (if (and (> (vector-length dired-entries) 0)
              (dired--parent-entry? (vector-ref dired-entries 0)))
         1 0)))

;; Marks by name, so a re-read of the same directory keeps them.
(define (dired--saved-marks)
  (map1 (lambda (e) (cons (dired--name e) (dired--mark e)))
        (filter (lambda (e) (not (string=? (dired--mark e) " ")))
                (vector->list dired-entries))))

(define (dired--restore-marks saved)
  (if (null? saved)
      '()
      (begin
        (let ((i (dired--index-of (car (car saved)) 0)))
          (if (>= i 0) (vector-set! (vector-ref dired-entries i) 8 (cdr (car saved))) '()))
        (dired--restore-marks (cdr saved)))))

;; ─── rendering ──────────────────────────────────────────────────────────────

(define (dired--line e)
  (string-append (dired--mark e) " "
                 (dired--mode e) " "
                 (dired--pad-left (dired--human (dired--size e)) 6) " "
                 (dired--date e) "  "
                 (dired--name e)
                 (cond ((dired--link? e) (string-append " -> " (dired--target e)))
                       ((dired--dir? e) "/")
                       (else ""))))

(define (dired--lines i acc)
  (if (< i 0)
      acc
      (dired--lines (- i 1) (cons (dired--line (vector-ref dired-entries i)) acc))))

(define (dired--summary)
  (let ((marked (length (dired--with-mark "*")))
        (flagged (length (dired--with-mark "D"))))
    (string-append "  " (dired--plural (dired--file-count) "entry" "entries")
                   ", sorted by " dired-sort-mode
                   (if dired-show-hidden ", dotfiles shown" "")
                   (if (> marked 0) (string-append ", " (number->string marked) " marked") "")
                   (if (> flagged 0) (string-append ", " (number->string flagged) " flagged") "")
                   "  —  ? for keys")))

(define (dired--index) (- (cursor-row) dired--header-rows))

(define (dired--current)
  (let ((i (dired--index)))
    (if (and (>= i 0) (< i (vector-length dired-entries)))
        (vector-ref dired-entries i)
        #f)))

(define (dired--goto-index i)
  (editor-goto-line (+ dired--header-rows
                       (max 0 (min i (- (vector-length dired-entries) 1))))
                    dired--name-column))

;; Redraws the buffer from dired-entries, then puts the cursor on entry i.
(define (dired--render i)
  (begin
    (editor-scratch-set-lines
     dired-buffer-id
     (cons (string-append "  " dired-dir ":")
           (cons (dired--summary)
                 (cons "" (dired--lines (- (vector-length dired-entries) 1) '())))))
    (dired--goto-index i)))

;; Lists `dir`, putting the cursor on `focus` (a name) if it's there, else
;; on entry `fallback`. Marks survive only when re-reading the same dir.
(define (dired--load dir focus fallback)
  (let ((saved (if (string=? dir dired-dir) (dired--saved-marks) '())))
    (begin
      (set! dired-dir dir)
      (set! dired-entries (dired--read dir))
      (dired--restore-marks saved)
      (let ((i (dired--index-of focus 0)))
        (dired--render (if (>= i 0) i fallback))))))

;; First real entry: skip ".." when there is one.
(define (dired--first-file-index)
  (if (and (> (vector-length dired-entries) 0)
           (dired--parent-entry? (vector-ref dired-entries 0)))
      1 0))

(define (dired--show dir focus)
  (begin
    (set! dired-buffer-id (editor-scratch-open "*dired*"))
    (editor-scratch-set-prefix dired-buffer-id "dired")
    (editor-switch-to-buffer dired-buffer-id)
    (set! dired-dir "")   ; a fresh visit: don't carry marks over
    (dired--load dir focus (if (string=? dir "/") 0 1))))

;; ─── entry points ───────────────────────────────────────────────────────────

(define (dired--directory? path)
  (let ((attrs (file-attributes path)))
    (and attrs (car attrs))))

(define (dired-open dir)
  (let ((path (expand-path dir)))
    (if (dired--directory? path)
        (dired--show path "")
        (editor-message (string-append "Not a directory: " path)))))

;; The current project's root when a project is open, else the working
;; directory.
(define (dired--project-dir)
  (let ((root (project-root)))
    (if (string=? root "") (editor-pwd) root)))

;; The current file's directory — except for buffers with no file, and the
;; splash screen (whose "file" lives in ~/.beluga/assets), where the project
;; root (or the working directory) is what you'd want, as with Emacs' *scratch*.
(define (dired--default-dir)
  (let* ((file (buffer-filename))
         (dir (if (string=? file "") "" (dired--dirname file))))
    (if (or (string=? dir "")
            (string=? dir (expand-path "~/.beluga/assets"))
            (not (dired--directory? dir)))
        (dired--project-dir)
        dir)))

;; Prompts from the project root when a project is open, like find-file;
;; dired-jump is the one that goes to the current file's directory.
(define (dired)
  (let* ((root (project-root))
         (start (if (string=? root "") (dired--default-dir) root))
         (answer (editor-prompt "Dired (directory): %u" start #t)))
    (if answer (dired-open answer) '())))

(define (dired-jump)
  (let ((file (buffer-filename))
        (dir (dired--default-dir)))
    (dired--show dir (if (and (not (string=? file ""))
                              (string=? dir (dired--dirname file)))
                         (dired--basename file)
                         ""))))

;; ─── commands inside the dired buffer ───────────────────────────────────────

(define (dired-next) (dired--goto-index (+ (max (dired--index) -1) 1)))
(define (dired-prev) (dired--goto-index (- (dired--index) 1)))

(define (dired-refresh)
  (let ((e (dired--current)))
    (dired--load dired-dir (if e (dired--name e) "") (dired--index))))

(define (dired-up)
  (if (string=? dired-dir "/")
      (editor-message "Already at /")
      (dired--load (dired--dirname dired-dir) (dired--basename dired-dir) 0)))

(define (dired-find-file)
  (let ((e (dired--current)))
    (cond ((not e) '())
          ((dired--parent-entry? e) (dired-up))
          ((dired--dir? e) (dired--load (dired--path e) "" (dired--first-file-index)))
          (else
           (begin
             (editor-open-file-path (dired--path e))
             ;; opening a new file buffer doesn't reset the key prefix:
             ;; leave "dired" or every key typed into the file is swallowed
             (editor-set-prefix "no-prefix"))))))

(define (dired--mark-at-point m)
  (let ((e (dired--current)) (i (dired--index)))
    (if (and e (not (dired--parent-entry? e)))
        (begin
          (vector-set! e 8 m)
          (dired--render (+ i 1)))
        (dired--goto-index (+ i 1)))))

(define (dired-mark) (dired--mark-at-point "*"))
(define (dired-unmark) (dired--mark-at-point " "))
(define (dired-flag-delete) (dired--mark-at-point "D"))

(define (dired-unmark-all)
  (begin
    (dired--for-each-file (lambda (e) (vector-set! e 8 " ")))
    (dired--render (dired--index))))

(define (dired-toggle-marks)
  (begin
    (dired--for-each-file
     (lambda (e)
       (cond ((string=? (dired--mark e) "*") (vector-set! e 8 " "))
             ((string=? (dired--mark e) " ") (vector-set! e 8 "*"))
             (else '()))))
    (dired--render (dired--index))))

;; Marked files, or the one under the cursor when nothing is marked.
(define (dired--targets)
  (let ((marked (dired--with-mark "*")))
    (if (null? marked)
        (let ((e (dired--current)))
          (if (and e (not (dired--parent-entry? e))) (list e) '()))
        marked)))

;; ─── file operations ────────────────────────────────────────────────────────
;; Each returns (exit-code . message): (0 . "") on success.

(define (dired--join-spaced strings)
  (cond ((null? strings) "")
        ((null? (cdr strings)) (car strings))
        (else (string-append (car strings) " " (dired--join-spaced (cdr strings))))))

;; ! — `command` run in the platform's shell (sh, or PowerShell on Windows)
;; from `dir`, with `names` appended as arguments, each quoted for it.
(define (dired--shell command dir names)
  (shell-run (string-append command " " (dired--join-spaced (map1 shell-quote names))) dir))

;; Runs (run entry) — which returns (exit-code . output) — for each entry,
;; collecting "name: error" strings.
(define (dired--run-each run entries failures)
  (if (null? entries)
      (reverse failures)
      (let ((result (run (car entries))))
        (dired--run-each run (cdr entries)
                         (if (= (car result) 0)
                             failures
                             (cons (string-append (dired--name (car entries)) ": "
                                                  (dired--first-line (cdr result)))
                                   failures))))))

(define (dired--delete entries)
  (if (null? entries)
      (editor-message "Nothing to delete")
      (if (dired--confirm? (string-append "Delete " (dired--describe entries)
                                          ;; a symlink to a directory is removed as a link
                                          (if (null? (filter (lambda (e) (and (dired--dir? e) (not (dired--link? e))))
                                                             entries))
                                              "?"
                                              ", directories recursively?")))
          (let ((failures (dired--run-each
                           (lambda (e) (delete-path (dired--path e)))
                           entries '())))
            (begin
              (dired--load dired-dir "" (dired--index))
              (editor-message (if (null? failures)
                                  (string-append "Deleted " (dired--describe entries))
                                  (string-append "Delete failed — " (car failures))))))
          (editor-message "Delete cancelled"))))

(define (dired-do-delete) (dired--delete (dired--targets)))
(define (dired-do-flagged-delete) (dired--delete (dired--with-mark "D")))

;; Rename and copy: one entry asks for its new path, several ask for a
;; destination directory. `transfer` is move-paths or copy-paths.
(define (dired--transfer verb past transfer)
  (let ((entries (dired--targets)))
    (if (null? entries)
        (editor-message (string-append "Nothing to " verb))
        (let* ((single (null? (cdr entries)))
               (question (if single
                             (string-append verb " " (dired--prompt-safe (dired--name (car entries))) " to: %u")
                             (string-append verb " " (dired--plural (length entries) "file" "files") " into: %u")))
               (answer (editor-prompt question
                                      (if single (dired--path (car entries)) (dired--dir-slash))
                                      #t)))
          (if answer
              (let* ((dest (expand-path answer dired-dir))
                     (result (transfer (map1 dired--path entries) dest)))
                (begin
                  (dired--load dired-dir (if single (dired--basename dest) "") (dired--index))
                  (editor-message (if (= (car result) 0)
                                      (string-append past " " (dired--describe entries) " to " dest)
                                      (string-append verb " failed — " (dired--first-line (cdr result)))))))
              (editor-message (string-append verb " cancelled")))))))

(define (dired-do-rename) (dired--transfer "Rename" "Moved" move-paths))
(define (dired-do-copy) (dired--transfer "Copy" "Copied" copy-paths))

(define (dired-create-directory)
  (let ((answer (editor-prompt "Create directory: %u" (dired--dir-slash) #t)))
    (if answer
        (let* ((path (expand-path answer dired-dir))
               (result (make-directories path)))
          (if (= (car result) 0)
              (begin
                (dired--load dired-dir (dired--basename path) (dired--index))
                (editor-message (string-append "Created " path)))
              (editor-message (string-append "Create directory failed — " (dired--first-line (cdr result))))))
        '())))

;; Runs a shell command from the listed directory with the target files as
;; its arguments: `wc -l` on two marked files runs `wc -l a.c b.c`.
(define (dired-shell-command)
  (let ((entries (dired--targets)))
    (if (null? entries)
        (editor-message "No file here")
        (let ((command (editor-prompt (string-append "! on " (dired--prompt-safe (dired--describe entries)) ": %u")
                                      "" #f)))
          (if (and command (not (string=? command "")))
              (let ((result (dired--shell command dired-dir (map1 dired--name entries))))
                (begin
                  (dired--load dired-dir "" (dired--index))
                  (editor-message
                   (string-append (if (= (car result) 0) "" (string-append "[exit " (number->string (car result)) "] "))
                                  (if (string=? (cdr result) "")
                                      "(no output)"
                                      (dired--first-line (cdr result)))))))
              '())))))

(define (dired-cycle-sort)
  (begin
    (set! dired-sort-mode (cond ((string=? dired-sort-mode "name") "date")
                                ((string=? dired-sort-mode "date") "size")
                                (else "name")))
    (dired-refresh)))

(define (dired-toggle-hidden)
  (begin
    (set! dired-show-hidden (not dired-show-hidden))
    (dired-refresh)))

(define (dired-quit)
  (if (>= dired-buffer-id 0) (editor-kill-buffer dired-buffer-id) '()))

(define (dired-help)
  (editor-message "RET open  ^ up  m/u/U mark  t toggle  d flag  x del flagged  D del  R rename  C copy  + mkdir  ! shell  s sort  . dotfiles  g refresh  q quit"))

(add-hook "buffer-killed"
          (lambda (name id)
            (if (= id dired-buffer-id)
                (begin
                  (set! dired-buffer-id -1)
                  (set! dired-dir ""))
                '())))

;; ─── keys ───────────────────────────────────────────────────────────────────

(map-key "d" dired "user")
(map-key "CTRL-j" dired-jump "user")

;; Sticky: the buffer is a menu, keys shouldn't drop back to "no-prefix"
;; after each press. ESCAPE (or CTRL-g) leaves it; CTRL-x still works.
(add-prefix "dired" #t)
(map-key "n" dired-next "dired")
(map-key "p" dired-prev "dired")
(map-key " " dired-next "dired")
(map-key "ARROW-DOWN" dired-next "dired")
(map-key "ARROW-UP" dired-prev "dired")
(map-key "PAGE-DOWN" move-cursor-page-down "dired")
(map-key "PAGE-UP" move-cursor-page-up "dired")
(map-key "ENTER" dired-find-file "dired")
(map-key "f" dired-find-file "dired")
(map-key "e" dired-find-file "dired")
(map-key "^" dired-up "dired")
(map-key "BACKSPACE" dired-up "dired")
(map-key "g" dired-refresh "dired")
(map-key "m" dired-mark "dired")
(map-key "u" dired-unmark "dired")
(map-key "U" dired-unmark-all "dired")
(map-key "t" dired-toggle-marks "dired")
(map-key "d" dired-flag-delete "dired")
(map-key "x" dired-do-flagged-delete "dired")
(map-key "D" dired-do-delete "dired")
(map-key "R" dired-do-rename "dired")
(map-key "C" dired-do-copy "dired")
(map-key "+" dired-create-directory "dired")
(map-key "!" dired-shell-command "dired")
(map-key "s" dired-cycle-sort "dired")
(map-key "." dired-toggle-hidden "dired")
(map-key "?" dired-help "dired")
(map-key "q" dired-quit "dired")
(map-key "CTRL-x" '(editor-set-prefix "user") "dired")
(map-key "ESCAPE" '(editor-set-prefix "no-prefix") "dired")
