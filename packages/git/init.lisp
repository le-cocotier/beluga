;; ─── Git ────────────────────────────────────────────────────────────────────
;; (add-package "git") — a small magit-style git manager.
;;
;;   CTRL-x g   git status of the current file's repository (or the working
;;              directory's), in the *git* buffer
;;
;; In *git* (sticky "git" prefix):
;;
;;   n / p, arrows      move            ENTER  open file / show commit / diff section
;;   s / S              stage file or section / stage everything
;;   u / U              unstage file or section / unstage everything
;;   k                  discard changes (asks; deletes an untracked file)
;;   =                  diff of the file or section under the cursor
;;   c                  commit staged changes: the message is edited in a
;;                      buffer (save to commit, CTRL-x k to cancel)
;;   a                  amend the last commit with the staged changes
;;   P / F / f          push / pull (fast-forward only) / fetch, in the background
;;   l                  log            b / B  switch branch / new branch
;;   g                  refresh        q      quit       ?  keys
;;
;; In the diff/log/commit viewer: arrows, n / p, PAGE-UP/DOWN to move, ENTER
;; on a log line shows that commit, q goes back to *git*. Diffs are colored.
;; On a diff of unstaged changes, s stages the hunk under the cursor and k
;; discards it (asks); on a diff of staged changes, u unstages it.
;;
;;   CTRL-x c l  the *packages* list: every package in ~/.beluga/packages,
;;               bundled / git / local / dev link, loaded or not, and for
;;               git ones the branch, local changes and ahead/behind.
;;               ENTER opens *git* on that package, g refresh, q quit.
;;
;; Push, pull and fetch run with process-start, so the editor stays usable
;; while they talk to the remote; everything else is local and quick, and
;; runs with process-run. Nothing can prompt for a password (the commands
;; have no terminal): use an SSH agent or a credential helper — an
;; authentication failure is reported in the status bar.

(define git-dir "")          ; top level of the repository being shown
(define git-buffer-id -1)
(define git-view-buffer-id -1)
(define git--items (list->vector '()))   ; one (kind . data) per *git* row
(define git--view-mode "")               ; "log" or "text", for ENTER in the viewer
(define git--view-hashes (list->vector '()))
(define git--max-lines 5000)  ; lines kept from any one command's output

;; ─── small helpers ──────────────────────────────────────────────────────────

(define (git--starts-with? s prefix)
  (and (>= (string-length s) (string-length prefix))
       (string=? (substring s 0 (string-length prefix)) prefix)))

(define (git--lines s)
  (if (string=? s "") '() (string-split s "\n" git--max-lines)))

(define (git--last lst)
  (if (null? (cdr lst)) (car lst) (git--last (cdr lst))))

;; Every git command: -C to target the repository without a cd, and
;; quotePath off so non-ASCII names come through as themselves.
(define (git--argv args)
  (append (list "git" "-C" git-dir "-c" "core.quotePath=false") args))

(define (git--run . args) (apply process-run (git--argv args)))
(define (git--ok? result) (= (car result) 0))

;; The line of git's output worth a status bar: the first error/fatal/
;; rejected line if there is one, else the last line.
(define (git--find-error lines)
  (cond ((null? lines) #f)
        ((or (git--starts-with? (car lines) "fatal:")
             (git--starts-with? (car lines) "error:")
             (git--starts-with? (car lines) " ! "))
         (car lines))
        (else (git--find-error (cdr lines)))))

(define (git--summary output)
  (let ((lines (git--lines output)))
    (if (null? lines)
        ""
        (let ((err (git--find-error lines)))
          (if err err (git--last lines))))))

(define (git--report result done)
  (editor-message (if (git--ok? result)
                      done
                      (string-append "git: " (git--summary (cdr result))))))

;; Prompts are format strings: keep a "%" in a file name from being read as
;; a directive (same trick as dired).
(define (git--safe s i start acc)
  (cond ((>= i (string-length s)) (string-append acc (substring s start i)))
        ((char=? (string-ref s i) #\%)
         (git--safe s (+ i 1) (+ i 1) (string-append acc (substring s start i) "％")))
        (else (git--safe s (+ i 1) start acc))))
(define (git--prompt-safe s) (git--safe s 0 0 ""))

(define (git--confirm? question)
  (let ((answer (editor-prompt (string-append (git--prompt-safe question) " (yes or no) %u") "" #f)))
    (and answer (string=? answer "yes"))))

;; git C-quotes names with spaces, quotes or control characters:
;; "with space.txt", "q\"uote.txt". A backslash (character code 92)
;; escapes the next character, which is taken as is — right for \" and
;; \\, the escapes a real file name gets; a tab or newline in a name
;; (\t, \n) would come out as the letter, but no one names files that way.
;; Written with character codes on purpose: this reader chokes on a
;; backslash character literal next to such strings.
(define (git--backslash? c) (= (char->integer c) 92))

(define (git--unescape s i acc)
  (cond ((>= i (- (string-length s) 1)) acc)   ; stop before the closing quote
        ((and (git--backslash? (string-ref s i)) (< (+ i 1) (string-length s)))
         (git--unescape s (+ i 2) (string-append acc (string (string-ref s (+ i 1))))))
        (else (git--unescape s (+ i 1) (string-append acc (string (string-ref s i)))))))

(define (git--unquote s)
  (if (and (> (string-length s) 1) (= (char->integer (string-ref s 0)) 34))   ; "
      (git--unescape s 1 "")
      s))

(define (git--pad s width)
  (if (< (string-length s) width)
      (string-append s (make-string (- width (string-length s)) #\space))
      s))

;; ─── reading the status ─────────────────────────────────────────────────────

(define (git--status-word c)
  (cond ((char=? c #\M) "modified")
        ((char=? c #\A) "new file")
        ((char=? c #\D) "deleted")
        ((char=? c #\R) "renamed")
        ((char=? c #\C) "copied")
        ((char=? c #\U) "conflict")
        ((char=? c #\T) "typechange")
        (else "changed")))

;; "XY path" or "XY old -> new" -> the path to act on.
(define (git--entry-path line)
  (let ((parts (string-split (substring line 3 (string-length line)) " -> ")))
    (git--unquote (git--last parts))))

;; Porcelain lines -> three lists of (word . path), in git's order.
(define (git--classify lines untracked unstaged staged)
  (if (null? lines)
      (list (reverse untracked) (reverse unstaged) (reverse staged))
      (let* ((line (car lines))
             (x (string-ref line 0))
             (y (string-ref line 1))
             (path (git--entry-path line)))
        (if (git--starts-with? line "??")
            (git--classify (cdr lines) (cons (cons "untracked" path) untracked) unstaged staged)
            (git--classify (cdr lines)
                           untracked
                           (if (char=? y #\space) unstaged (cons (cons (git--status-word y) path) unstaged))
                           (if (char=? x #\space) staged (cons (cons (git--status-word x) path) staged)))))))

;; ─── the *git* buffer ───────────────────────────────────────────────────────
;; Built as two parallel lists — display lines and one (kind . data) item
;; per line — so the row under the cursor always says what it is.

(define git--out-lines '())
(define git--out-items '())

(define (git--emit line item)
  (begin
    (set! git--out-lines (cons line git--out-lines))
    (set! git--out-items (cons item git--out-items))))

(define (git--emit-files kind entries)
  (if (null? entries)
      '()
      (begin
        (git--emit (string-append "   " (git--pad (car (car entries)) 11) (cdr (car entries)))
                   (cons kind (cdr (car entries))))
        (git--emit-files kind (cdr entries)))))

(define (git--emit-section title kind entries)
  (if (null? entries)
      '()
      (begin
        (git--emit (string-append " " title " (" (number->string (length entries)) ")")
                   (cons "section" kind))
        (git--emit-files kind entries)
        (git--emit "" (cons "none" "")))))

(define (git--emit-commits lines)
  (if (null? lines)
      '()
      (begin
        (git--emit (string-append "   " (car lines))
                   (cons "commit" (car (string-split (car lines) " "))))
        (git--emit-commits (cdr lines)))))

(define (git--render)
  (let* ((status (git--run "status" "--porcelain=v1" "-b"))
         (lines (git--lines (cdr status)))
         (head (if (and (pair? lines) (git--starts-with? (car lines) "## "))
                   (substring (car lines) 3 (string-length (car lines)))
                   "?"))
         (entries (if (pair? lines) (cdr lines) '()))
         (groups (git--classify entries '() '() '()))
         (log (git--run "log" "--oneline" "-n" "10")))
    (begin
      (set! git--out-lines '())
      (set! git--out-items '())
      (git--emit (string-append " Repo:  " git-dir) (cons "none" ""))
      (git--emit (string-append " Head:  " head) (cons "none" ""))
      (git--emit "" (cons "none" ""))
      (if (null? entries)
          (begin (git--emit " Working tree clean" (cons "none" ""))
                 (git--emit "" (cons "none" "")))
          (begin
            (git--emit-section "Untracked files" "untracked" (car groups))
            (git--emit-section "Unstaged changes" "unstaged" (car (cdr groups)))
            (git--emit-section "Staged changes" "staged" (car (cdr (cdr groups))))))
      (if (git--ok? log)
          (begin
            (git--emit " Recent commits" (cons "none" ""))
            (git--emit-commits (git--lines (cdr log))))
          '())
      (git--emit "" (cons "none" ""))
      (git--emit " ? for keys" (cons "none" ""))
      (set! git--items (list->vector (reverse git--out-items)))
      (editor-scratch-set-lines git-buffer-id (reverse git--out-lines)))))

(define (git--item-at row)
  (if (and (>= row 0) (< row (vector-length git--items)))
      (vector-ref git--items row)
      (cons "none" "")))

(define (git--item) (git--item-at (cursor-row)))

(define (git--item-row item i)
  (cond ((>= i (vector-length git--items)) -1)
        ((and (string=? (car (vector-ref git--items i)) (car item))
              (string=? (cdr (vector-ref git--items i)) (cdr item)))
         i)
        (else (git--item-row item (+ i 1)))))

(define (git--first-file-row i)
  (cond ((>= i (vector-length git--items)) 0)
        ((member (car (vector-ref git--items i)) (list "untracked" "unstaged" "staged")) i)
        (else (git--first-file-row (+ i 1)))))

;; Re-reads the repository. The cursor stays on the same item (or row) —
;; but only when *git* is on screen: a background push finishing while
;; you're in another buffer mustn't move your cursor there.
(define (git-refresh)
  (if (< git-buffer-id 0)
      '()
      (let ((visible (= (current-buffer-id) git-buffer-id))
            (item (git--item))
            (row (cursor-row)))
        (begin
          (git--render)
          (if visible
              ;; a blank/header row matches the first such row: keep the row
              (let ((found (if (string=? (car item) "none") -1 (git--item-row item 0))))
                (editor-goto-line (if (>= found 0) found row) 0))
              '())))))

(define (git--toplevel dir) (process-run "git" "-C" dir "rev-parse" "--show-toplevel"))

(define (git--open dir)
  (let ((top (git--toplevel dir)))
    (if (git--ok? top)
        (begin
          (set! git-dir (car (git--lines (cdr top))))
          (set! git-buffer-id (editor-scratch-open "*git*"))
          (editor-scratch-set-prefix git-buffer-id "git")
          (editor-switch-to-buffer git-buffer-id)
          (git--render)
          (editor-goto-line (git--first-file-row 0) 0))
        (editor-message (string-append "Not in a git repository: " dir)))))

(define (git-status-dir dir) (git--open (expand-path dir)))

;; The current file's repository — or, when that file isn't in one (the
;; splash screen lives in ~/.beluga/assets) or there's no file at all, the
;; working directory's, the same fallback dired uses.
(define (git-status)
  (let* ((file (buffer-filename))
         (file-dir (if (string=? file "") "" (expand-path ".." file))))
    (git--open (if (and (not (string=? file-dir ""))
                        (git--ok? (git--toplevel file-dir)))
                   file-dir
                   (editor-pwd)))))

;; ─── staging ────────────────────────────────────────────────────────────────

(define (git--paths-of kind)
  (map1 cdr (filter (lambda (item) (string=? (car item) kind)) (vector->list git--items))))

(define (git--after result done)
  (begin
    (git-refresh)
    (git--report result done)))

(define (git-stage)
  (let ((item (git--item)))
    (cond ((member (car item) (list "untracked" "unstaged"))
           (git--after (git--run "add" "--" (cdr item)) (string-append "Staged " (cdr item))))
          ((and (string=? (car item) "section") (string=? (cdr item) "unstaged"))
           (git--after (git--run "add" "-u") "Staged all changes"))
          ((and (string=? (car item) "section") (string=? (cdr item) "untracked"))
           (git--after (apply git--run (append (list "add" "--") (git--paths-of "untracked")))
                       "Staged all untracked files"))
          (else (editor-message "Nothing to stage here")))))

(define (git-stage-all) (git--after (git--run "add" "-A") "Staged everything"))

(define (git-unstage)
  (let ((item (git--item)))
    (cond ((string=? (car item) "staged")
           (git--after (git--run "restore" "--staged" "--" (cdr item))
                       (string-append "Unstaged " (cdr item))))
          ((and (string=? (car item) "section") (string=? (cdr item) "staged"))
           (git--after (git--run "reset" "-q") "Unstaged everything"))
          (else (editor-message "Nothing to unstage here")))))

(define (git-unstage-all) (git--after (git--run "reset" "-q") "Unstaged everything"))

(define (git-discard)
  (let ((item (git--item)))
    (cond ((string=? (car item) "unstaged")
           (if (git--confirm? (string-append "Discard your changes to " (cdr item) "?"))
               (git--after (git--run "restore" "--" (cdr item)) (string-append "Discarded changes to " (cdr item)))
               (editor-message "Kept")))
          ((string=? (car item) "untracked")
           (if (git--confirm? (string-append "Delete untracked " (cdr item) "?"))
               (git--after (git--run "clean" "-fdq" "--" (cdr item)) (string-append "Deleted " (cdr item)))
               (editor-message "Kept")))
          ((string=? (car item) "staged") (editor-message "Unstage it first (u)"))
          (else (editor-message "Nothing to discard here")))))

;; ─── committing ─────────────────────────────────────────────────────────────

;; c opens .git/COMMIT_EDITMSG in a normal buffer, so the message can have
;; a body: save it (CTRL-x CTRL-s) to commit, kill it (CTRL-x k) to cancel.
;; Lines starting with # are dropped, like git's own editor flow. If the
;; commit fails, for an empty message say, the buffer stays open: fix it
;; and save again.
(define git--commit-file "")

(define (git--commit-template)
  (string-append
   "\n"
   "# Write the commit message above: a summary line, a blank line, then\n"
   "# the body. Save (CTRL-x CTRL-s) to commit, or kill this buffer\n"
   "# (CTRL-x k) to cancel. Lines starting with # are left out.\n"
   "#\n"
   "# Staged:\n"
   (apply string-append
          (map1 (lambda (p) (string-append "#   " p "\n")) (git--paths-of "staged")))))

(define (git-commit)
  (if (null? (git--paths-of "staged"))
      (editor-message "Nothing staged — s to stage a file, S for everything")
      (let ((gitdir (git--run "rev-parse" "--absolute-git-dir")))
        (if (git--ok? gitdir)
            (let ((file (string-append (car (git--lines (cdr gitdir))) "/COMMIT_EDITMSG")))
              (if (write-file file (git--commit-template))
                  (begin
                    (set! git--commit-file file)
                    (editor-open-file-path file)
                    (editor-set-prefix "no-prefix")
                    (editor-message "Commit message: save to commit, CTRL-x k to cancel"))
                  (editor-message (string-append "Couldn't write " file))))
            (git--report gitdir "")))))

(define (git--first-message-line lines)
  (cond ((null? lines) "")
        ((or (string=? (car lines) "") (git--starts-with? (car lines) "#"))
         (git--first-message-line (cdr lines)))
        (else (car lines))))

(add-hook "after-save"
          (lambda (filename)
            (if (and (not (string=? git--commit-file ""))
                     (string=? (expand-path filename) git--commit-file))
                (let ((result (git--run "commit" "-q" "--cleanup=strip" "-F" git--commit-file)))
                  (begin
                    (if (git--ok? result)
                        (begin
                          (set! git--commit-file "")
                          (editor-kill-buffer (current-buffer-id))
                          (if (>= git-buffer-id 0) (editor-switch-to-buffer git-buffer-id) '())
                          (git-refresh)
                          (editor-message
                           (string-append "Committed: "
                                          (git--first-message-line
                                           (git--lines (cdr (git--run "log" "-1" "--format=%B")))))))
                        (git--report result ""))))
                '())))

(define (git-amend)
  (if (git--confirm? "Amend the last commit with the staged changes (its message is kept)?")
      (git--after (git--run "commit" "-q" "--amend" "--no-edit") "Amended the last commit")
      (editor-message "Amend cancelled")))

;; ─── remote operations (background) ─────────────────────────────────────────

;; process-start takes the argv as separate arguments, not a list.
(define (git--start callback argv) (apply process-start (cons callback argv)))

(define (git--background label args done)
  (if (git--start (lambda (result)
                    (begin
                      (git-refresh)
                      (git--report result done)))
                  (git--argv args))
      (editor-message (string-append label "…"))
      (editor-message "Couldn't start git (too many background processes?)")))

(define (git--current-branch)
  (let ((r (git--run "rev-parse" "--abbrev-ref" "HEAD")))
    (if (git--ok? r) (car (git--lines (cdr r))) "HEAD")))

(define (git-push)
  (if (git--ok? (git--run "rev-parse" "--abbrev-ref" "--symbolic-full-name" "@{u}"))
      (git--background "Pushing" (list "push") "Pushed")
      ;; No upstream yet: ask where, and set it (push -u).
      (let ((answer (editor-prompt "Push to (remote branch): %u"
                                   (string-append "origin " (git--current-branch)) #f)))
        (if answer
            (let ((parts (string-split answer " ")))
              (if (= (length parts) 2)
                  (git--background "Pushing" (list "push" "-u" (car parts) (car (cdr parts)))
                                   (string-append "Pushed to " answer))
                  (editor-message "Expected: <remote> <branch>")))
            (editor-message "Push cancelled")))))

(define (git-pull) (git--background "Pulling" (list "pull" "--ff-only") "Pulled"))
(define (git-fetch) (git--background "Fetching" (list "fetch" "--prune") "Fetched"))

;; ─── branches ───────────────────────────────────────────────────────────────

(define (git-switch-branch)
  (let ((r (git--run "branch" "--format=%(refname:short)")))
    (if (git--ok? r)
        (editor-list-popup "Branch"
                           (map1 (lambda (b) (cons b b)) (git--lines (cdr r)))
                           (lambda (branch)
                             (git--after (git--run "switch" branch)
                                         (string-append "On branch " branch)))
                           '()
                           #t)
        (git--report r ""))))

(define (git-new-branch)
  (let ((name (editor-prompt "New branch (from HEAD): %u" "" #f)))
    (if name
        (git--after (git--run "switch" "-c" name) (string-append "Created and switched to " name))
        '())))

;; ─── viewer: diffs, the log, a commit ───────────────────────────────────────

;; Diffs are colored by whole lines: syntax-add-line-prefix rules on a
;; "diff" language, which the viewer buffer is set to (buffer-set-syntax).
;; Longer prefixes first: "+++" must win over "+".
(syntax-define-language "diff" "" "" "" "")
(syntax-add-line-prefix "diff" "diff --git" "type")
(syntax-add-line-prefix "diff" "+++ " "type")
(syntax-add-line-prefix "diff" "--- " "type")
(syntax-add-line-prefix "diff" "index " "comment")
(syntax-add-line-prefix "diff" "commit " "keyword")
(syntax-add-line-prefix "diff" "@@" "number")
(syntax-add-line-prefix "diff" "+" "added")
(syntax-add-line-prefix "diff" "-" "removed")

;; What the viewer shows, so hunks can be staged from it: "unstaged" or
;; "staged" for a diff of the work tree or the index, "" otherwise; the
;; git arguments that produced it (to redraw after staging a hunk); and
;; its lines.
(define git--view-kind "")
(define git--view-args '())
(define git--view-title "")
(define git--view-lines (list->vector '()))

(define (git--show-lines title lines mode hashes)
  (begin
    (set! git-view-buffer-id (editor-scratch-open "*git-view*"))
    (editor-scratch-set-prefix git-view-buffer-id "git-view")
    (buffer-set-syntax git-view-buffer-id (if (string=? mode "log") "" "diff"))
    (editor-switch-to-buffer git-view-buffer-id)
    (set! git--view-mode mode)
    (set! git--view-kind "")
    (set! git--view-lines (list->vector lines))
    (set! git--view-hashes (list->vector (cons "" (cons "" hashes))))
    (editor-scratch-set-lines git-view-buffer-id
                              (cons (string-append " " title) (cons "" lines)))
    (editor-goto-line (if (null? lines) 0 2) 0)))

(define (git--show-output title result)
  (if (git--ok? result)
      (let ((lines (git--lines (cdr result))))
        (git--show-lines title (if (null? lines) (list "(no differences)") lines) "text" '()))
      (git--report result "")))

(define (git-diff)
  (let ((item (git--item)))
    (cond ((string=? (car item) "unstaged")
           (git--show-diff (string-append "Unstaged: " (cdr item) "   (s stage hunk, k discard hunk)")
                           "unstaged" (list "diff" "--" (cdr item))))
          ((string=? (car item) "staged")
           (git--show-diff (string-append "Staged: " (cdr item) "   (u unstage hunk)")
                           "staged" (list "diff" "--cached" "--" (cdr item))))
          ((string=? (car item) "untracked")
           ;; exits 1 when there's a difference, which here is always
           (let ((r (git--run "diff" "--no-index" "--" "/dev/null" (cdr item))))
             (git--show-lines (string-append "Untracked: " (cdr item)) (git--lines (cdr r)) "text" '())))
          ((and (string=? (car item) "section") (string=? (cdr item) "unstaged"))
           (git--show-diff "Unstaged changes   (s stage hunk, k discard hunk)" "unstaged" (list "diff")))
          ((and (string=? (car item) "section") (string=? (cdr item) "staged"))
           (git--show-diff "Staged changes   (u unstage hunk)" "staged" (list "diff" "--cached")))
          ((string=? (car item) "commit") (git--show-commit (cdr item)))
          (else (editor-message "Nothing to diff here")))))

(define (git--show-diff title kind args)
  (let ((result (apply git--run args)))
    (begin
      (git--show-output title result)
      (if (git--ok? result)
          (begin
            (set! git--view-kind kind)
            (set! git--view-args args)
            (set! git--view-title title))
          '()))))

(define (git--show-commit hash)
  (git--show-output (string-append "Commit " hash) (git--run "show" "--stat" "--patch" hash)))

;; Log lines carry a graph prefix ("* | "), so the hash is the first word
;; made only of hex digits, at least 7 long.
(define (git--hex? s i)
  (cond ((>= i (string-length s)) #t)
        ((let ((c (string-ref s i)))
           (or (char-numeric? c) (and (char>=? c #\a) (char<=? c #\f))))
         (git--hex? s (+ i 1)))
        (else #f)))

(define (git--first-hash words)
  (cond ((null? words) "")
        ((and (>= (string-length (car words)) 7) (git--hex? (car words) 0)) (car words))
        (else (git--first-hash (cdr words)))))

(define (git-log)
  (let ((r (git--run "log" "--graph" "--decorate" "--format=%h %ad %an%d  %s" "--date=short" "-n" "300")))
    (if (git--ok? r)
        (let ((lines (git--lines (cdr r))))
          (git--show-lines (string-append "Log — " (git--current-branch) "  (ENTER shows a commit)")
                           lines "log"
                           (map1 (lambda (l) (git--first-hash (string-split l " "))) lines)))
        (git--report r ""))))

(define (git-view-enter)
  (if (string=? git--view-mode "log")
      (let ((row (cursor-row)))
        (if (< row (vector-length git--view-hashes))
            (let ((hash (vector-ref git--view-hashes row)))
              (if (string=? hash "") '() (git--show-commit hash)))
            '()))
      '()))

(define (git-view-quit)
  (begin
    (if (>= git-view-buffer-id 0) (editor-kill-buffer git-view-buffer-id) '())
    (if (>= git-buffer-id 0)
        (begin (editor-switch-to-buffer git-buffer-id) (git-refresh))
        '())))

;; ─── hunks ──────────────────────────────────────────────────────────────────
;; A hunk is applied on its own by giving `git apply` a patch made of its
;; file's header (from "diff --git" to the first "@@") and the hunk itself.

(define (git--line i) (vector-ref git--view-lines i))

;; Index of the closest line at or above i that starts with prefix, stopping
;; (with -1) at a line starting with stop.
(define (git--find-up i prefix stop)
  (cond ((< i 0) -1)
        ((git--starts-with? (git--line i) prefix) i)
        ((and stop (git--starts-with? (git--line i) stop)) -1)
        (else (git--find-up (- i 1) prefix stop))))

;; First line after i starting a new hunk or file (or the end).
(define (git--hunk-end i)
  (cond ((>= i (vector-length git--view-lines)) i)
        ((or (git--starts-with? (git--line i) "@@")
             (git--starts-with? (git--line i) "diff --git"))
         i)
        (else (git--hunk-end (+ i 1)))))

(define (git--first-hunk i)
  (if (or (>= i (vector-length git--view-lines)) (git--starts-with? (git--line i) "@@"))
      i
      (git--first-hunk (+ i 1))))

(define (git--join-lines from to acc)
  (if (>= from to)
      acc
      (git--join-lines (+ from 1) to (string-append acc (git--line from) "\n"))))

;; The patch for the hunk under the cursor, or #f.
(define (git--hunk-patch)
  (let ((i (- (cursor-row) 2)))            ; the viewer has a title and a blank line
    (if (or (< i 0) (>= i (vector-length git--view-lines)))
        #f
        (let ((start (git--find-up i "@@" "diff --git")))
          (if (< start 0)
              #f
              (let ((file (git--find-up start "diff --git" #f)))
                (if (< file 0)
                    #f
                    (string-append (git--join-lines file (git--first-hunk file) "")
                                   (git--join-lines start (git--hunk-end (+ start 1)) "")))))))))

(define (git--apply-hunk args done)
  (let ((patch (git--hunk-patch))
        (gitdir (git--run "rev-parse" "--absolute-git-dir")))
    (if (not patch)
        (editor-message "Not on a hunk: move the cursor into one (below its @@ line)")
        (let ((file (string-append (car (git--lines (cdr gitdir))) "/beluga-hunk.patch")))
          (if (write-file file patch)
              (let ((result (apply git--run (append args (list file)))))
                (begin
                  (delete-path file)
                  (if (git--ok? result)
                      (let ((row (cursor-row)))
                        (begin
                          (git--show-diff git--view-title git--view-kind git--view-args)
                          (editor-goto-line row 0)
                          (editor-message done)))
                      (git--report result ""))))
              (editor-message (string-append "Couldn't write " file)))))))

(define (git-view-stage-hunk)
  (if (string=? git--view-kind "unstaged")
      (git--apply-hunk (list "apply" "--cached") "Staged the hunk")
      (editor-message "s stages a hunk from an unstaged diff (= on an unstaged file)")))

(define (git-view-unstage-hunk)
  (if (string=? git--view-kind "staged")
      (git--apply-hunk (list "apply" "--cached" "-R") "Unstaged the hunk")
      (editor-message "u unstages a hunk from a staged diff (= on a staged file)")))

(define (git-view-discard-hunk)
  (cond ((not (string=? git--view-kind "unstaged"))
         (editor-message "k discards a hunk from an unstaged diff"))
        ((git--confirm? "Discard this hunk from the work tree?")
         (git--apply-hunk (list "apply" "-R") "Discarded the hunk"))
        (else (editor-message "Discard cancelled"))))

;; ─── the *packages* list ────────────────────────────────────────────────────
;; Every package in ~/.beluga/packages: where it comes from, whether it's
;; loaded, and for the ones under git their branch, local changes and
;; commits ahead of / behind their upstream. ENTER opens *git* on the
;; package's repository (for a --dev link, the beluga checkout itself).

(define git-packages-buffer-id -1)
(define git--package-dirs (list->vector '()))   ; one directory (or "") per row

(define (git--col s width) (git--pad (string-append s " ") width))

(define (git--in dir . args) (apply process-run (append (list "git" "-C" dir) args)))

(define (git--package-kind name dir attrs)
  (cond ((string=? name "personal-config") "personal")
        ((car (cdr attrs)) "dev link")                    ; install.sh --dev
        ((file-attributes (string-append dir "/.git")) "git")
        ((file-attributes (string-append dir "/.bundled")) "bundled")
        (else "local")))

;; "ahead 2, behind 1" from `rev-list --left-right --count @{u}...HEAD`
;; ("1\t2"), or "" when there's no upstream.
(define (git--ahead-behind dir)
  (let ((r (git--in dir "rev-list" "--left-right" "--count" "@{u}...HEAD")))
    (if (git--ok? r)
        (let ((counts (string-split (car (git--lines (cdr r))) "\t")))
          (if (= (length counts) 2)
              (let ((behind (car counts)) (ahead (car (cdr counts))))
                (cond ((and (string=? ahead "0") (string=? behind "0")) "up to date")
                      ((string=? behind "0") (string-append "ahead " ahead))
                      ((string=? ahead "0") (string-append "behind " behind))
                      (else (string-append "ahead " ahead ", behind " behind))))
              ""))
        "")))

;; Branch, changes and sync for a directory inside a git work tree; #f
;; when it isn't in one.
(define (git--package-repo-state dir)
  (let ((branch (git--in dir "rev-parse" "--abbrev-ref" "HEAD")))
    (if (git--ok? branch)
        (let ((changes (git--in dir "status" "--porcelain" "--" ".")))
          (list (car (git--lines (cdr branch)))
                (if (and (git--ok? changes) (not (string=? (cdr changes) ""))) "modified" "clean")
                (git--ahead-behind dir)))
        #f)))

(define (git--package-row name)
  (let* ((dir (expand-path (string-append "~/.beluga/packages/" name)))
         (attrs (file-attributes dir))
         (kind (git--package-kind name dir attrs))
         (loaded (if (package-loaded? name) "yes" "no"))
         (repo (if (member kind (list "git" "dev link")) (git--package-repo-state dir) #f)))
    (cons (string-append " " (git--col name 20) (git--col kind 10) (git--col loaded 8)
                         (if repo
                             (string-append (git--col (car repo) 16) (git--col (car (cdr repo)) 10)
                                            (car (cdr (cdr repo))))
                             ""))
          (if repo dir ""))))

(define (git--package-names)
  (sort (map1 car (filter (lambda (entry) (cdr entry))
                          (list-directory (expand-path "~/.beluga/packages"))))
        string<?))

(define (git--packages-render)
  (let ((rows (map1 git--package-row (git--package-names))))
    (begin
      (set! git--package-dirs (list->vector (append (list "" "") (map1 cdr rows))))
      (editor-scratch-set-lines
       git-packages-buffer-id
       (append (list (string-append " " (git--col "Package" 20) (git--col "Kind" 10) (git--col "Loaded" 8)
                                    (git--col "Branch" 16) (git--col "Changes" 10) "Upstream")
                     "")
               (map1 car rows)
               (list "" " ENTER git status   g refresh   q quit"))))))

(define (git-packages)
  (begin
    (if (< git-packages-buffer-id 0)
        (begin
          (set! git-packages-buffer-id (editor-scratch-open "*packages*"))
          (editor-scratch-set-prefix git-packages-buffer-id "packages"))
        '())
    (editor-switch-to-buffer git-packages-buffer-id)
    (editor-message "Reading packages…")
    (git--packages-render)
    (editor-goto-line 2 0)
    (editor-message "")))

(define (git-packages-refresh)
  (let ((row (cursor-row)))
    (begin (git--packages-render) (editor-goto-line row 0))))

(define (git-packages-open)
  (let ((row (cursor-row)))
    (if (and (< row (vector-length git--package-dirs))
             (not (string=? (vector-ref git--package-dirs row) "")))
        (git-status-dir (vector-ref git--package-dirs row))
        (editor-message "Not under git: nothing to show"))))

(define (git-packages-quit)
  (if (>= git-packages-buffer-id 0) (editor-kill-buffer git-packages-buffer-id) '()))

;; ─── misc ───────────────────────────────────────────────────────────────────

(define (git-open-at-point)
  (let ((item (git--item)))
    (cond ((member (car item) (list "untracked" "unstaged" "staged"))
           (begin
             (editor-open-file-path (string-append git-dir "/" (cdr item)))
             (editor-set-prefix "no-prefix")))
          (else (git-diff)))))

(define (git-quit)
  (if (>= git-buffer-id 0) (editor-kill-buffer git-buffer-id) '()))

(define (git-help)
  (editor-message "s/S stage  u/U unstage  k discard  = diff (s/u/k there: hunks)  c commit  a amend  P push  F pull  f fetch  l log  b/B branch  g refresh  q quit"))

(add-hook "buffer-killed"
          (lambda (name id)
            (cond ((= id git-buffer-id) (set! git-buffer-id -1))
                  ((= id git-view-buffer-id) (set! git-view-buffer-id -1))
                  ((= id git-packages-buffer-id) (set! git-packages-buffer-id -1))
                  (else '()))))

;; ─── keys ───────────────────────────────────────────────────────────────────

(map-key "g" git-status "user")

(add-prefix "git" #t)
(map-key "n" '(move-cursor "down") "git")
(map-key "p" '(move-cursor "up") "git")
(map-key "ARROW-DOWN" '(move-cursor "down") "git")
(map-key "ARROW-UP" '(move-cursor "up") "git")
(map-key "PAGE-DOWN" move-cursor-page-down "git")
(map-key "PAGE-UP" move-cursor-page-up "git")
(map-key "ENTER" git-open-at-point "git")
(map-key "s" git-stage "git")
(map-key "S" git-stage-all "git")
(map-key "u" git-unstage "git")
(map-key "U" git-unstage-all "git")
(map-key "k" git-discard "git")
(map-key "=" git-diff "git")
(map-key "c" git-commit "git")
(map-key "a" git-amend "git")
(map-key "P" git-push "git")
(map-key "F" git-pull "git")
(map-key "f" git-fetch "git")
(map-key "l" git-log "git")
(map-key "b" git-switch-branch "git")
(map-key "B" git-new-branch "git")
(map-key "g" git-refresh "git")
(map-key "q" git-quit "git")
(map-key "?" git-help "git")
(map-key "CTRL-x" '(editor-set-prefix "user") "git")
(map-key "ESCAPE" '(editor-set-prefix "no-prefix") "git")

(add-prefix "git-view" #t)
(map-key "n" '(move-cursor "down") "git-view")
(map-key "p" '(move-cursor "up") "git-view")
(map-key "ARROW-DOWN" '(move-cursor "down") "git-view")
(map-key "ARROW-UP" '(move-cursor "up") "git-view")
(map-key "PAGE-DOWN" move-cursor-page-down "git-view")
(map-key "PAGE-UP" move-cursor-page-up "git-view")
(map-key "ENTER" git-view-enter "git-view")
(map-key "s" git-view-stage-hunk "git-view")
(map-key "u" git-view-unstage-hunk "git-view")
(map-key "k" git-view-discard-hunk "git-view")
(map-key "q" git-view-quit "git-view")
(map-key "CTRL-x" '(editor-set-prefix "user") "git-view")
(map-key "ESCAPE" '(editor-set-prefix "no-prefix") "git-view")

(map-key "l" git-packages "config")   ; CTRL-x c l
(add-prefix "packages" #t)
(map-key "n" '(move-cursor "down") "packages")
(map-key "p" '(move-cursor "up") "packages")
(map-key "ARROW-DOWN" '(move-cursor "down") "packages")
(map-key "ARROW-UP" '(move-cursor "up") "packages")
(map-key "ENTER" git-packages-open "packages")
(map-key "g" git-packages-refresh "packages")
(map-key "q" git-packages-quit "packages")
(map-key "CTRL-x" '(editor-set-prefix "user") "packages")
(map-key "ESCAPE" '(editor-set-prefix "no-prefix") "packages")
