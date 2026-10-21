;; ─── Package ────────────────────────────────────────────────────────────────
;; (add-package "package") — installs, updates and removes packages.
;; Loaded first by ~/.beluga/config/init.lisp, so (use-package ...) works
;; anywhere after it; your declarations belong in personal-config.
;;
;; DECLARING (in ~/.beluga/packages/personal-config/init.lisp)
;;
;;   (use-package "outline")                        bundled, or already installed
;;   (use-package "rainbow" "https://host/rainbow.git")           default branch
;;   (use-package "rainbow" "https://host/rainbow.git" "v1.2.0")  tag, branch or commit
;;   (use-package "rainbow" "https://host/rainbow.git" "latest")  highest vX.Y.Z tag
;;   (use-package "mine" "~/code/beluga-mine")      a local folder, loaded in place
;;
;; A declared package that isn't installed is cloned while beluga starts
;; (it says "Installing …"), then loaded where it's declared, so the order
;; of your use-package lines is the load order. Nothing else at startup
;; touches the network. Without a URL, a package is looked up in the index
;; (package-index-url, below).
;;
;; THE LOCKFILE: ~/.beluga/packages/personal-config/packages.lock
;;
;; Records the exact commit of every package installed from git. On another
;; machine with the same personal-config folder, the same commits are
;; installed. Written by the commands below; keep it with your dotfiles.
;; Bundled packages and local folders are never in it.
;;
;; COMMANDS (M-x, or keys in the *packages* list)
;;
;;   package-list       CTRL-x c l: every package, its version, commit, state
;;   package-install    clone a package and load it (asks for name and URL)
;;   package-update     fetch, show the new commits, ask, check them out
;;   package-update-all the same for every git package, one question
;;   package-rollback   back to the commit before the last update
;;   package-remove     move a package to ~/.beluga/packages.bak
;;   package-sync       install what's declared, move to the locked commits,
;;                      report packages nothing declares any more
;;   package-search     search the index, ENTER installs
;;   package-index-refresh   fetch the index again
;;
;; Updates, removals and rollbacks apply on the next start: a loaded
;; package's keys and hooks can't be taken back. `beluga --sync-packages`
;; runs package-sync without opening the editor.
;;
;; THE MANIFEST: package.lisp, next to a package's init.lisp, optional
;;
;;   (define-package "git-blame"
;;     (version "1.2.0")
;;     (description "Blame annotations in the gutter")
;;     (requires ("beluga" "3.4") ("git" "3.3"))
;;     (sources ("git" "https://host/beluga-git.git")))
;;
;; Read, never evaluated. add-package loads what `requires` names first and
;; refuses a package needing a newer beluga; installing a package installs
;; its missing requirements, from `sources` or the index.
;;
;; Everything that runs git is a chain of callbacks: in the background
;; (process-start) once the editor is up, in the foreground (process-run)
;; while init.lisp loads and in batch mode, where background processes
;; would report back too late, or never.

(define package-index-url #f)         ; a git repository with an index.lisp
(define package-remove-deletes #f)    ; #t: package-remove deletes instead of moving to packages.bak
(define package--timeout 600000)      ; foreground git commands: 10 minutes

(define package--declared '())   ; (name source ref), in declaration order
(define package--lock '())       ; (name (field . value) ...)
(define package--lock-read? #f)
(define package--updates '())    ; (name . "N new commits") after a check, for the list
(define package--problems '())   ; the last messages that went wrong, newest first
(define package-list-buffer-id -1)
(define package-update-buffer-id -1)
(define package--rows (list->vector '()))

;; ─── small helpers ──────────────────────────────────────────────────────────

(define (package--starts-with? s prefix)
  (and (>= (string-length s) (string-length prefix))
       (string=? (substring s 0 (string-length prefix)) prefix)))

(define (package--lines s)
  (if (string=? s "") '() (string-split s "\n" 5000)))

(define (package--first-line s)
  (let ((lines (filter (lambda (l) (not (string=? l ""))) (package--lines s))))
    (if (null? lines) "" (car lines))))

(define (package--ok? result) (= (car result) 0))

(define (package--pad s width)
  (if (< (string-length s) width)
      (string-append s (make-string (- width (string-length s)) #\space))
      (string-append (substring s 0 (- width 1)) " ")))

(define (package--short commit)
  (if (and (string? commit) (> (string-length commit) 7)) (substring commit 0 7) (if commit commit "")))

(define (package--join strings sep)
  (cond ((null? strings) "")
        ((null? (cdr strings)) (car strings))
        (else (string-append (car strings) sep (package--join (cdr strings) sep)))))

(define (package--remove-name name alist)
  (filter (lambda (entry) (not (string=? (car entry) name))) alist))

;; Startup, and beluga --sync-packages: run git in the foreground.
(define (package--foreground?)
  (or (editor-batch?) (not (editor-initialized?))))

(define (package--say msg)
  (begin
    (editor-message msg)
    (if (package--foreground?) (editor-redisplay) '())))

(define (package--fail msg)
  (begin
    (set! package--problems (cons msg package--problems))
    (if (> (length package--problems) 10)
        (set! package--problems (list-tail package--problems (- (length package--problems) 10)))
        '())
    (package--say msg)))

;; Prompts are format strings: keep a "%" in a name from being read as a
;; directive (same trick as dired and git).
(define (package--safe s i start acc)
  (cond ((>= i (string-length s)) (string-append acc (substring s start i)))
        ((char=? (string-ref s i) #\%)
         (package--safe s (+ i 1) (+ i 1) (string-append acc (substring s start i) "％")))
        (else (package--safe s (+ i 1) start acc))))

;; Nothing can be asked in batch mode or while init.lisp loads: what's
;; written in the config is the answer.
(define (package--confirm? question)
  (if (package--foreground?)
      #t
      (let ((answer (editor-prompt (string-append (package--safe question 0 0 "") " (yes or no) %u") "" #f)))
        (and answer (string=? answer "yes")))))

(define (package--ask question initial)
  (let ((answer (editor-prompt (string-append question "%u") initial #f)))
    (if (and answer (not (string=? answer ""))) answer #f)))

;; ─── paths ──────────────────────────────────────────────────────────────────

(define (package--dir name) (package-directory name))
(define (package--staging-dir) (string-append (package-directory) "/.staging"))
(define (package--backup-dir) (string-append (beluga-directory) "/packages.bak"))
(define (package--lock-path) (string-append (package-directory "personal-config") "/packages.lock"))
(define (package--index-dir) (string-append (beluga-directory) "/index"))

(define (package--exists? path) (if (file-attributes path) #t #f))
(define (package--installed? name) (package--exists? (string-append (package--dir name) "/init.lisp")))
(define (package--git? name) (package--exists? (string-append (package--dir name) "/.git")))
(define (package--bundled? name) (package--exists? (string-append (package--dir name) "/.bundled")))
(define (package--link? name)
  (let ((attrs (file-attributes (package--dir name))))
    (and attrs (car (cdr attrs)))))

;; A source is a local folder rather than a git URL when it is a path:
;; "/...", "~...", "./...", "../..." or "C:/...".
(define (package--local-path? source)
  (or (package--starts-with? source "/")
      (package--starts-with? source "~")
      (package--starts-with? source ".")
      (and (> (string-length source) 2)
           (char=? (string-ref source 1) #\:)
           (char-alphabetic? (string-ref source 0)))))

;; ─── git ────────────────────────────────────────────────────────────────────

;; (k (exit-code . "output")), now or later.
(define (package--run k argv)
  (if (package--foreground?)
      (k (apply process-run (cons package--timeout argv)))
      (if (apply process-start (cons k argv))
          '()
          (k (cons 127 "couldn't start git")))))

(define (package--git dir k . args)
  (package--run k (append (list "git" "-C" dir "-c" "advice.detachedHead=false") args)))

(define (package--git-now dir . args)
  (apply process-run (append (list "git" "-C" dir) args)))

(define (package--git-error result)
  (let ((lines (filter (lambda (l) (or (package--starts-with? l "fatal:")
                                       (package--starts-with? l "error:")))
                       (package--lines (cdr result)))))
    (if (null? lines) (package--first-line (cdr result)) (car lines))))

(define (package--head dir)
  (let ((r (package--git-now dir "rev-parse" "HEAD")))
    (if (package--ok? r) (package--first-line (cdr r)) #f)))

(define (package--origin dir)
  (let ((r (package--git-now dir "remote" "get-url" "origin")))
    (if (package--ok? r) (package--first-line (cdr r)) #f)))

(define (package--dirty? dir)
  (let ((r (package--git-now dir "status" "--porcelain")))
    (and (package--ok? r) (not (string=? (cdr r) "")))))

(define (package--version-tag? tag)
  (let ((s (if (or (package--starts-with? tag "v") (package--starts-with? tag "V"))
               (substring tag 1 (string-length tag))
               tag)))
    (and (> (string-length s) 0) (char-numeric? (string-ref s 0)))))

(define (package--highest-tag tags best)
  (cond ((null? tags) best)
        ((not (package--version-tag? (car tags))) (package--highest-tag (cdr tags) best))
        ((or (not best) (> (version-compare (car tags) best) 0)) (package--highest-tag (cdr tags) (car tags)))
        (else (package--highest-tag (cdr tags) best))))

;; The commit `ref` names in a clone, after a clone or a fetch: #f is the
;; remote's default branch, "latest" the highest version tag, a branch
;; name its tip on the remote, anything else a tag or a commit.
(define (package--resolve dir ref)
  (let ((rev (cond ((not ref) "origin/HEAD")
                   ((string=? ref "latest")
                    (let ((tags (package--git-now dir "tag" "--list")))
                      (package--highest-tag (if (package--ok? tags) (package--lines (cdr tags)) '()) #f)))
                   ((package--ok? (package--git-now dir "rev-parse" "--verify" "--quiet"
                                                    (string-append "refs/remotes/origin/" ref)))
                    (string-append "origin/" ref))
                   (else ref))))
    (if rev
        (let ((r (package--git-now dir "rev-parse" "--verify" "--quiet" (string-append rev "^{commit}"))))
          (if (package--ok? r) (package--first-line (cdr r)) #f))
        #f)))

(define (package--has-commit? dir commit)
  (package--ok? (package--git-now dir "cat-file" "-e" (string-append commit "^{commit}"))))

;; ─── the lockfile ───────────────────────────────────────────────────────────
;; One (package "name" (field "value") ...) per line; in memory
;; ("name" (field . "value") ...).

(define (package--form->entry form)
  (if (and (pair? form) (eq? (car form) 'package) (pair? (cdr form)) (string? (second form)))
      (cons (second form)
            (map1 (lambda (f) (cons (car f) (second f)))
                  (filter (lambda (f) (and (pair? f) (symbol? (car f)) (pair? (cdr f))))
                          (cdr (cdr form)))))
      #f))

(define (package--entry->form entry)
  (cons 'package
        (cons (car entry)
              (map1 (lambda (f) (list (car f) (cdr f)))
                    (filter (lambda (f) (cdr f)) (cdr entry))))))

(define (package--lock-load)
  (if package--lock-read?
      '()
      (let ((forms (read-data-file (package--lock-path))))
        (begin
          (set! package--lock (filter (lambda (e) e) (map1 package--form->entry (if forms forms '()))))
          (set! package--lock-read? #t)))))

(define (package--lock-save)
  (begin
    (make-directories (package--dir "personal-config"))
    (if (write-data-file (package--lock-path)
                         (map1 package--entry->form
                               (sort package--lock (lambda (a b) (string<? (car a) (car b)))))
                         ";; Written by beluga's package manager: the exact commit of every package\n;; installed from git. Keep it with personal-config to get the same packages\n;; on another machine (beluga --sync-packages).\n")
        #t
        (package--fail (string-append "package: couldn't write " (package--lock-path))))))

(define (package--locked name)
  (begin (package--lock-load) (assoc name package--lock)))

(define (package--lock-get name field)
  (let ((entry (package--locked name)))
    (if entry
        (let ((f (assq field (cdr entry)))) (if f (cdr f) #f))
        #f)))

;; Merges `fields` ((field . value) ...) into name's entry, and saves.
(define (package--lock-set! name fields)
  (let* ((entry (package--locked name))
         (old (if entry (cdr entry) '()))
         (kept (filter (lambda (f) (not (assq (car f) fields))) old)))
    (begin
      (set! package--lock (cons (cons name (append fields kept)) (package--remove-name name package--lock)))
      (package--lock-save))))

(define (package--lock-drop! name)
  (begin
    (package--lock-load)
    (set! package--lock (package--remove-name name package--lock))
    (package--lock-save)))

;; ─── declarations and manifests ─────────────────────────────────────────────

(define (package--declare! name source ref)
  (set! package--declared
        (append (package--remove-name name package--declared) (list (list name source ref)))))

(define (package--declaration name) (assoc name package--declared))
(define (package--declared-source name)
  (let ((d (package--declaration name))) (if d (second d) #f)))
(define (package--declared-ref name)
  (let ((d (package--declaration name))) (if d (third d) #f)))

;; The fields of dir/package.lisp's (define-package "name" ...), or '().
(define (package--manifest-in dir)
  (let ((forms (read-data-file (string-append dir "/package.lisp"))))
    (let ((found (filter (lambda (f) (and (pair? f) (eq? (car f) 'define-package) (pair? (cdr f))))
                         (if forms forms '()))))
      (if (null? found) '() (cdr (cdr (car found)))))))

(define (package--manifest name) (package--manifest-in (package--dir name)))

(define (package--field manifest field)
  (let ((f (filter (lambda (c) (and (pair? c) (eq? (car c) field))) manifest)))
    (if (null? f) '() (cdr (car f)))))

(define (package--field-string manifest field)
  (let ((values (package--field manifest field)))
    (if (and (pair? values) (string? (car values))) (car values) #f)))

;; ((dep min-version) ...), beluga itself left out.
(define (package--requires name)
  (filter (lambda (r) (and (pair? r) (string? (car r)) (not (string=? (car r) "beluga"))))
          (package--field (package--manifest name) 'requires)))

(define (package--manifest-source name dep)
  (let ((s (assoc dep (filter (lambda (s) (and (pair? s) (string? (car s)) (pair? (cdr s))))
                              (package--field (package--manifest name) 'sources)))))
    (if s (second s) #f)))

;; ─── the index ──────────────────────────────────────────────────────────────
;; package-index-url is a git repository holding an index.lisp of
;;   (package "name" (url "https://...") (description "..."))
;; forms. It's cloned to ~/.beluga/index on first use.

(define (package--index)
  (let ((forms (read-data-file (string-append (package--index-dir) "/index.lisp"))))
    (filter (lambda (e) e) (map1 package--form->entry (if forms forms '())))))

(define (package--index-field entry field)
  (let ((f (assq field (cdr entry)))) (if f (cdr f) #f)))

(define (package--index-ensure k)
  (cond ((not package-index-url) (k #f))
        ((package--exists? (string-append (package--index-dir) "/.git")) (k #t))
        (else
         (begin
           (package--say "Fetching the package index…")
           (package--run (lambda (r)
                           (if (package--ok? r)
                               (k #t)
                               (begin (package--fail (string-append "package: couldn't fetch the index: "
                                                                    (package--git-error r)))
                                      (k #f))))
                         (list "git" "clone" "--quiet" package-index-url (package--index-dir)))))))

(define (package-index-refresh)
  (if (package--exists? (string-append (package--index-dir) "/.git"))
      (package--git (package--index-dir)
                    (lambda (r)
                      (if (package--ok? r)
                          (package--say (string-append "Package index: "
                                                       (number->string (length (package--index))) " packages"))
                          (package--fail (string-append "package: couldn't update the index: "
                                                        (package--git-error r)))))
                    "pull" "--quiet" "--ff-only")
      (package--index-ensure
       (lambda (ok)
         (if ok
             (package--say (string-append "Package index: " (number->string (length (package--index))) " packages"))
             (if (not package-index-url)
                 (package--fail "package: no index: (define package-index-url \"https://...\") in personal-config")
                 '()))))))

;; (k url-or-#f), fetching the index first when there is one to fetch.
(define (package--index-url name k)
  (package--index-ensure
   (lambda (ok)
     (let ((entry (assoc name (package--index))))
       (k (if entry (package--index-field entry 'url) #f))))))

;; ─── sequencing ─────────────────────────────────────────────────────────────

;; (f item next) for each item, one after the other; (k all-ok?) at the end.
(define (package--each items f k)
  (package--each-from items f k #t))

(define (package--each-from items f k ok)
  (if (null? items)
      (k ok)
      (f (car items) (lambda (item-ok) (package--each-from (cdr items) f k (and ok item-ok))))))

;; ─── installing ─────────────────────────────────────────────────────────────

;; Installs the missing packages `name` requires, then (k ok).
(define (package--ensure-requires name k)
  (package--each (package--requires name)
                 (lambda (req next)
                   (if (package--installed? (car req))
                       (next #t)
                       (package--install (car req) (package--manifest-source name (car req)) #f name next)))
                 k))

;; Clones `name` into the staging directory, checks out the locked commit
;; (when the lock has one for the same URL) or `ref`, moves it into place,
;; records it in the lock, installs what it requires, then (k ok). `via`
;; is the package it's a dependency of, or #f.
(define (package--install name source ref via k)
  (cond ((package--installed? name) (k #t))
        ((and (not source) (package--lock-get name 'url))
         (package--install name (package--lock-get name 'url) (or ref (package--lock-get name 'ref)) via k))
        ((not source)
         (package--index-url name
                             (lambda (url)
                               (if url
                                   (package--install name url ref via k)
                                   (begin
                                     (package--fail (string-append "package: \"" name "\" isn't installed and has no URL"
                                                                   (if via (string-append " (needed by " via ")") "")))
                                     (k #f))))))
        ((package--exists? (package--dir name))
         (begin (package--fail (string-append "package: " (package--dir name) " exists but has no init.lisp"))
                (k #f)))
        (else (package--clone name source ref via k))))

(define (package--clone name url ref via k)
  (let ((staging (string-append (package--staging-dir) "/" name))
        (locked (if (equal? (package--lock-get name 'url) url) (package--lock-get name 'commit) #f)))
    (begin
      (make-directories (package--staging-dir))
      (if (package--exists? staging) (delete-path staging) '())
      (package--say (string-append "Installing " name "…"))
      (package--run
       (lambda (r)
         (if (not (package--ok? r))
             (begin (package--fail (string-append "package: couldn't clone " name ": " (package--git-error r)))
                    (delete-path staging)
                    (k #f))
             (let ((commit (if (and locked (package--has-commit? staging locked)) locked (package--resolve staging ref))))
               (if (not commit)
                   (begin (package--fail (string-append "package: " name " has no " (if ref ref "default branch")))
                          (delete-path staging)
                          (k #f))
                   (package--git staging
                                 (lambda (r)
                                   (if (not (package--ok? r))
                                       (begin (package--fail (string-append "package: " name ": " (package--git-error r)))
                                              (delete-path staging)
                                              (k #f))
                                       (package--clone-done name url ref via commit staging k)))
                                 "checkout" "--quiet" "--detach" commit)))))
       (list "git" "clone" "--quiet" url staging)))))

(define (package--clone-done name url ref via commit staging k)
  (let ((moved (move-paths (list staging) (package--dir name))))
    (if (not (package--ok? moved))
        (begin (package--fail (string-append "package: couldn't move " name " into place: " (cdr moved)))
               (k #f))
        (begin
          (package--lock-set! name (list (cons 'url url) (cons 'ref ref) (cons 'commit commit)
                                         (cons 'via (or via (package--lock-get name 'via)))))
          (package--ensure-requires
           name
           (lambda (ok)
             (begin
               (package--say (string-append "Installed " name " (" (package--short commit) ")"))
               (k ok))))))))

;; ─── declaring ──────────────────────────────────────────────────────────────

(define (package--load name dir)
  (if dir (add-package name dir) (add-package name)))

;; The declared ref changed since the package was installed (a new tag
;; in personal-config, say): check out what it names, if the clone already
;; has it, before loading. No fetch at startup: package-sync fetches.
(define (package--follow-ref name ref)
  (let ((dir (package--dir name)))
    (if (and (package--git? name) (package--locked name)
             (not (equal? ref (package--lock-get name 'ref))))
        (let ((target (package--resolve dir ref))
              (current (package--head dir)))
          (cond ((not target)
                 (package--fail (string-append "package: " name " has no " (if ref ref "default branch")
                                               " yet: run package-sync to fetch it")))
                ((package--dirty? dir)
                 (package--fail (string-append "package: " name " has local changes, not switching it to "
                                               (if ref ref "its default branch"))))
                ((equal? target current)
                 (package--lock-set! name (list (cons 'ref ref))))
                ((package--ok? (package--git-now dir "-c" "advice.detachedHead=false" "checkout" "--quiet" "--detach" target))
                 (begin
                   (package--lock-set! name (list (cons 'ref ref) (cons 'commit target) (cons 'previous current)))
                   (package--say (string-append name ": now at " (if ref ref "its default branch")
                                                " (" (package--short target) ")"))))
                (else (package--fail (string-append "package: couldn't switch " name " to " (if ref ref "its default branch"))))))
        '())))

;; (use-package "name" ["git-url" or "local/path"] ["ref"])
(define (use-package name . rest)
  (let ((source (if (pair? rest) (car rest) #f))
        (ref (if (and (pair? rest) (pair? (cdr rest))) (second rest) #f)))
    (begin
      (package--declare! name source ref)
      (cond ((and source (package--local-path? source))
             (package--load name (expand-path source)))
            ((package--installed? name)
             (begin
               (package--follow-ref name ref)
               (package--ensure-requires name (lambda (ok) (package--load name #f)))))
            (else
             (package--install name source ref #f
                               (lambda (ok) (if ok (package--load name #f) #f))))))))

;; ─── commands ───────────────────────────────────────────────────────────────

;; The package on the *packages* row under the cursor, else asked for.
(define (package--target question)
  (let ((row (if (and (>= package-list-buffer-id 0) (= (current-buffer-id) package-list-buffer-id))
                 (cursor-row) -1)))
    (if (and (>= row 0) (< row (vector-length package--rows)) (vector-ref package--rows row))
        (vector-ref package--rows row)
        (package--ask question ""))))

(define (package-install . args)
  (let* ((name (if (pair? args) (car args) (package--ask "Install package: " "")))
         (url (cond ((not name) #f)
                    ((and (pair? args) (pair? (cdr args))) (second args))
                    ((package--declared-source name) (package--declared-source name))
                    ((and (not (pair? args)) (not (package--foreground?)))
                     (package--ask (string-append "URL of " name " (empty: from the index): ") ""))
                    (else #f)))
         (ref (if (and (pair? args) (pair? (cdr args)) (pair? (cdr (cdr args)))) (third args) #f)))
    (cond ((not name) #f)
          ((package--installed? name) (package--say (string-append name " is already installed")))
          ((and url (not (package--foreground?))
                (not (package--lock-get name 'url))
                (not (package--confirm? (string-append "Install " name " from " url
                                                       "? It's code that runs inside beluga"))))
           (package--say "Not installed"))
          (else
           (package--install name url ref #f
                             (lambda (ok)
                               (begin
                                 (if ok (package--load name #f) #f)
                                 (if (and ok (not (package--declaration name)))
                                     (package--say (string-append "Installed " name ": add (use-package \"" name "\""
                                                                  (if url (string-append " \"" url "\"") "")
                                                                  ") to personal-config to keep it"))
                                     '())
                                 (package--list-refresh))))))))

;; (k plan): a plan is (name dir current target ref log-lines); 'current
;; when the package is already at its target, #f when it couldn't be
;; checked (reported).
(define (package--check name k)
  (let ((dir (package--dir name)))
    (cond ((not (package--git? name))
           (begin (package--fail (string-append "package: " name " isn't a git package"
                                                (if (package--bundled? name) " (bundled: update beluga)" "")))
                  (k #f)))
          ((package--dirty? dir)
           (begin (package--fail (string-append "package: " name " has local changes, not updating it"))
                  (k #f)))
          (else
           (package--git dir
                         (lambda (r)
                           (if (not (package--ok? r))
                               (begin (package--fail (string-append "package: couldn't fetch " name ": " (package--git-error r)))
                                      (k #f))
                               (let* ((ref (if (package--declaration name) (package--declared-ref name) (package--lock-get name 'ref)))
                                      (current (package--head dir))
                                      (target (package--resolve dir ref)))
                                 (cond ((not target)
                                        (begin (package--fail (string-append "package: " name " has no " (if ref ref "default branch")))
                                               (k #f)))
                                       ((equal? current target)
                                        (begin (set! package--updates (package--remove-name name package--updates))
                                               (k 'current)))
                                       (else
                                        (let ((log (package--git-now dir "log" "--oneline" "--no-decorate"
                                                                     (string-append current ".." target))))
                                          (let ((lines (if (package--ok? log) (package--lines (cdr log)) '())))
                                            (begin
                                              (set! package--updates
                                                    (cons (cons name (if (null? lines)
                                                                         "older commit"
                                                                         (string-append (number->string (length lines)) " new")))
                                                          (package--remove-name name package--updates)))
                                              (k (list name dir current target ref lines))))))))))
                         "fetch" "--quiet" "--tags" "origin")))))

(define (package--apply plan k)
  (let ((name (first plan)) (dir (second plan)) (current (third plan))
        (target (list-ref plan 3)) (ref (list-ref plan 4)))
    (package--git dir
                  (lambda (r)
                    (if (not (package--ok? r))
                        (begin (package--fail (string-append "package: couldn't update " name ": " (package--git-error r)))
                               (k #f))
                        (begin
                          (package--lock-set! name (list (cons 'url (or (package--lock-get name 'url) (package--origin dir)))
                                                         (cons 'ref ref) (cons 'commit target) (cons 'previous current)))
                          (set! package--updates (package--remove-name name package--updates))
                          (package--ensure-requires name (lambda (ok) (k #t))))))
                  "checkout" "--quiet" "--detach" target)))

(define (package--show-plans plans)
  (if (package--foreground?)
      '()
      (begin
        (if (< package-update-buffer-id 0)
            (begin
              (set! package-update-buffer-id (editor-scratch-open "*package-update*"))
              (editor-scratch-set-prefix package-update-buffer-id "package-update"))
            '())
        (editor-switch-to-buffer package-update-buffer-id)
        (editor-scratch-set-lines
         package-update-buffer-id
         (apply append
                (map1 (lambda (plan)
                        (append (list (string-append " " (first plan) "  " (package--short (third plan))
                                                     " -> " (package--short (list-ref plan 3))))
                                (map1 (lambda (l) (string-append "     " l)) (list-ref plan 5))
                                (list "")))
                      plans)))
        (editor-goto-line 0 0)
        (editor-redisplay))))

(define (package--close-update-buffer)
  (begin
    (if (>= package-update-buffer-id 0) (editor-kill-buffer package-update-buffer-id) '())
    (if (>= package-list-buffer-id 0) (editor-switch-to-buffer package-list-buffer-id) '())))

(define (package--apply-plans plans k)
  (package--each plans package--apply
                 (lambda (ok)
                   (begin
                     (package--say (string-append "Updated " (package--join (map1 first plans) ", ")
                                                  (if (package--foreground?) "" ": restart beluga to use the new code")))
                     (k ok)))))

(define (package-update . args)
  (let ((name (if (pair? args) (car args) (package--target "Update package: "))))
    (if name
        (begin
          (package--say (string-append "Checking " name "…"))
          (package--check name
                          (lambda (plan)
                            (cond ((eq? plan 'current)
                                   (begin (package--say (string-append name " is up to date"))
                                          (package--list-refresh)))
                                  ((not plan) (package--list-refresh))
                                  ((begin (package--show-plans (list plan))
                                          (package--confirm? (string-append "Update " name "?")))
                                   (begin (package--close-update-buffer)
                                          (package--apply-plans (list plan) (lambda (ok) (package--list-refresh)))))
                                  (else (begin (package--close-update-buffer)
                                               (package--say "Not updated")
                                               (package--list-refresh)))))))
        #f)))

(define (package--git-packages)
  (filter (lambda (name) (and (package--git? name) (not (package--link? name))))
          (package--installed-names)))

(define (package-update-all)
  (let ((plans '()))
    (begin
      (package--say "Checking every package…")
      (package--each (package--git-packages)
                     (lambda (name next)
                       (package--check name (lambda (plan)
                                              (begin (if (pair? plan) (set! plans (append plans (list plan))) '())
                                                     (next #t)))))
                     (lambda (ok)
                       (cond ((null? plans) (begin (package--say "Every package is up to date") (package--list-refresh)))
                             ((begin (package--show-plans plans)
                                     (package--confirm? (string-append "Update " (package--join (map1 first plans) ", ") "?")))
                              (begin (package--close-update-buffer)
                                     (package--apply-plans plans (lambda (ok) (package--list-refresh)))))
                             (else (begin (package--close-update-buffer)
                                          (package--say "Not updated")
                                          (package--list-refresh)))))))))

;; Fetches every git package and marks the ones with new commits in the
;; list, without changing anything.
(define (package-check-updates)
  (begin
    (package--say "Checking every package…")
    (package--each (package--git-packages)
                   (lambda (name next) (package--check name (lambda (plan) (next #t))))
                   (lambda (ok)
                     (begin
                       (package--say (if (null? package--updates)
                                         "Every package is up to date"
                                         (string-append "Updates: " (package--join (map1 car package--updates) ", "))))
                       (package--list-refresh))))))

(define (package-rollback . args)
  (let ((name (if (pair? args) (car args) (package--target "Roll back package: "))))
    (cond ((not name) #f)
          ((not (package--lock-get name 'previous))
           (package--fail (string-append "package: no earlier commit of " name " to go back to")))
          ((package--dirty? (package--dir name))
           (package--fail (string-append "package: " name " has local changes, not rolling it back")))
          ((package--confirm? (string-append "Roll " name " back to " (package--short (package--lock-get name 'previous)) "?"))
           (let ((previous (package--lock-get name 'previous))
                 (current (package--head (package--dir name))))
             (package--git (package--dir name)
                           (lambda (r)
                             (if (package--ok? r)
                                 (begin
                                   (package--lock-set! name (list (cons 'commit previous) (cons 'previous current)))
                                   (package--say (string-append "Rolled " name " back to " (package--short previous)
                                                                ": restart beluga to use it"))
                                   (package--list-refresh))
                                 (package--fail (string-append "package: " (package--git-error r)))))
                           "checkout" "--quiet" "--detach" previous)))
          (else (package--say "Not rolled back")))))

;; Packages the lock says were installed for `name` that nothing else
;; installed requires, and that aren't declared themselves.
(define (package--orphans-of name)
  (filter (lambda (dep)
            (and (equal? (package--lock-get dep 'via) name)
                 (not (package--declaration dep))
                 (null? (filter (lambda (other) (and (not (string=? other name))
                                                     (assoc dep (package--requires other))))
                                (package--installed-names)))))
          (map1 car package--lock)))

(define (package-remove . args)
  (let ((name (if (pair? args) (car args) (package--target "Remove package: "))))
    (cond ((not name) #f)
          ((string=? name "personal-config") (package--fail "package: personal-config is yours, not removing it"))
          ((not (package--exists? (package--dir name))) (package--fail (string-append "package: " name " isn't installed")))
          ((package--confirm? (string-append "Remove " name (if package-remove-deletes " (deleted)" " (moved to packages.bak)") "?"))
           (let ((result (if package-remove-deletes
                             (delete-path (package--dir name))
                             (begin
                               (make-directories (package--backup-dir))
                               (move-paths (list (package--dir name))
                                           (string-append (package--backup-dir) "/" name "-"
                                                          (number->string (get-universal-time))))))))
             (if (package--ok? result)
                 (let ((orphans (begin (package--lock-load) (package--orphans-of name))))
                   (begin
                     (package--lock-drop! name)
                     (package--say (string-append "Removed " name
                                                  (if (package--declaration name)
                                                      ": delete its use-package line too, or the next start reinstalls it"
                                                      ": restart beluga to unload it")
                                                  (if (null? orphans)
                                                      ""
                                                      (string-append ". No longer needed: " (package--join orphans ", ")))))
                     (package--list-refresh)))
                 (package--fail (string-append "package: couldn't remove " name ": " (cdr result))))))
          (else (package--say "Not removed")))))

;; Brings one declared package to its locked commit (or installs it).
(define (package--sync-one decl next)
  (let* ((name (first decl)) (source (second decl)) (ref (third decl))
         (dir (package--dir name)) (locked (package--lock-get name 'commit)))
    (cond ((and source (package--local-path? source)) (next #t))
          ((not (package--installed? name)) (package--install name source ref #f next))
          ((not (package--git? name)) (package--ensure-requires name next))
          ((not locked)
           ;; Cloned by hand, or by an older use-package: adopt it.
           (begin
             (package--lock-set! name (list (cons 'url (or source (package--origin dir))) (cons 'ref ref)
                                            (cons 'commit (package--head dir))))
             (package--ensure-requires name next)))
          ((not (equal? ref (package--lock-get name 'ref)))
           ;; A ref the clone didn't have at startup: fetch it, then switch.
           (package--git dir (lambda (r)
                               (begin (package--follow-ref name ref)
                                      (if (equal? ref (package--lock-get name 'ref))
                                          (package--ensure-requires name next)
                                          (next #f))))
                         "fetch" "--quiet" "--tags" "origin"))
          ((equal? (package--head dir) locked) (package--ensure-requires name next))
          ((package--dirty? dir)
           (begin (package--fail (string-append "package: " name " has local changes, left at its commit"))
                  (next #f)))
          (else
           (let ((checkout (lambda ()
                             (package--git dir
                                           (lambda (r)
                                             (if (package--ok? r)
                                                 (begin (package--say (string-append name ": now at " (package--short locked)))
                                                        (package--ensure-requires name next))
                                                 (begin (package--fail (string-append "package: " name ": " (package--git-error r)))
                                                        (next #f))))
                                           "checkout" "--quiet" "--detach" locked))))
             (if (package--has-commit? dir locked)
                 (checkout)
                 (package--git dir (lambda (r) (checkout)) "fetch" "--quiet" "--tags" "origin")))))))

;; Installs every declared package that's missing, moves the others to
;; their locked commit, and reports installed packages that nothing
;; declares or requires. #t when everything went fine (in the
;; background, the result comes as a message).
(define (package-sync)
  (let ((result #t))
    (begin
      (package--lock-load)
      (package--each (filter (lambda (d) (not (package--bundled? (first d)))) package--declared)
                     package--sync-one
                     (lambda (ok)
                       (let ((orphans (package--undeclared)))
                         (begin
                           (set! result ok)
                           (package--say (string-append (if ok "Packages in sync" "Packages synced, with problems (see the *packages* list)")
                                                        (if (null? orphans)
                                                            ""
                                                            (string-append ". Not declared: " (package--join orphans ", ")))))
                           (package--list-refresh)))))
      result)))

;; Installed git packages neither declared nor required by an installed one.
(define (package--undeclared)
  (let ((installed (package--installed-names)))
    (filter (lambda (name)
              (and (package--git? name)
                   (not (package--declaration name))
                   (null? (filter (lambda (other) (assoc name (package--requires other))) installed))))
            installed)))

(define (package-search)
  (package--index-ensure
   (lambda (ok)
     (if (not ok)
         (if package-index-url '()
             (package--fail "package: no index: (define package-index-url \"https://...\") in personal-config"))
         (editor-list-popup
          "Packages"
          (map1 (lambda (entry)
                  (cons (string-append (car entry)
                                       (if (package--installed? (car entry)) " [installed]" "")
                                       (let ((d (package--index-field entry 'description))) (if d (string-append " — " d) "")))
                        (car entry)))
                (package--index))
          (lambda (name) (package-install name (package--index-field (assoc name (package--index)) 'url)))
          '()
          #t)))))

;; ─── the *packages* list ────────────────────────────────────────────────────

(define (package--installed-names)
  (sort (filter (lambda (name) (not (package--starts-with? name ".")))
                (map1 car (filter (lambda (entry) (cdr entry)) (list-directory (package-directory)))))
        string<?))

(define (package--kind name)
  (cond ((string=? name "personal-config") "personal")
        ((package--link? name) "dev link")
        ((package--git? name) "git")
        ((package--bundled? name) "bundled")
        (else "local")))

(define (package--state name)
  (let ((s (package-state name)))
    (cond ((not s) "-")
          ((string=? s "loaded") "yes")
          (else s))))

(define (package--row name)
  (let* ((kind (package--kind name))
         (manifest (package--manifest name))
         (version (or (package-version name) (package--field-string manifest 'version)
                      (if (string=? kind "bundled") (beluga-version) #f)))
         (git (string=? kind "git"))
         (commit (if git (package--short (package--head (package--dir name))) ""))
         (locked (package--lock-get name 'commit))
         (update (assoc name package--updates))
         (status (cond ((not git) "")
                       ((package--dirty? (package--dir name)) "modified")
                       (update (string-append (cdr update) " commits"))
                       ((and locked (not (string=? (package--short locked) commit))) "not at locked commit")
                       (else ""))))
    (string-append " " (package--pad name 20) (package--pad kind 10) (package--pad (or version "") 10)
                   (package--pad (package--state name) 8)
                   (package--pad (or (package--lock-get name 'ref) "") 12) (package--pad commit 9) status)))

(define (package--declared-missing)
  (filter (lambda (d) (and (not (package--installed? (first d)))
                           (not (and (second d) (package--local-path? (second d))))))
          package--declared))

(define (package--list-render)
  (let* ((names (package--installed-names))
         (paths (filter (lambda (d) (and (second d) (package--local-path? (second d)))) package--declared))
         (missing (package--declared-missing))
         (header (list (string-append " " (package--pad "Package" 20) (package--pad "Kind" 10) (package--pad "Version" 10)
                                      (package--pad "Loaded" 8) (package--pad "Ref" 12) (package--pad "Commit" 9) "Status")
                       ""))
         (rows (map1 package--row names))
         (path-rows (map1 (lambda (d) (string-append " " (package--pad (first d) 20) (package--pad "path" 10)
                                                     (package--pad (or (package-version (first d)) "") 10)
                                                     (package--pad (package--state (first d)) 8) (second d)))
                          paths))
         (missing-rows (map1 (lambda (d) (string-append " " (package--pad (first d) 20) (package--pad "missing" 10)
                                                        (package--pad "" 10) (package--pad "-" 8)
                                                        (if (second d) (second d) "")))
                             missing))
         (footer (append (list "" " ENTER git status  i install  u update  U update all  f check updates"
                               "     r roll back  d remove  s sync  / search  ? manifest  g refresh  q quit")
                         (if (null? package--problems)
                             '()
                             (cons "" (map1 (lambda (p) (string-append " ! " p)) (reverse package--problems)))))))
    (begin
      (set! package--rows (list->vector (append (list #f #f) names (map1 first paths) (map1 first missing)
                                                (map1 (lambda (l) #f) footer))))
      (editor-scratch-set-lines package-list-buffer-id (append header rows path-rows missing-rows footer)))))

(define (package--list-refresh)
  (if (>= package-list-buffer-id 0)
      (let ((row (if (= (current-buffer-id) package-list-buffer-id) (cursor-row) 2)))
        (begin (package--list-render)
               (if (= (current-buffer-id) package-list-buffer-id) (editor-goto-line row 0) '())))
      '()))

(define (package-list)
  (begin
    (package--lock-load)
    (if (< package-list-buffer-id 0)
        (begin
          (set! package-list-buffer-id (editor-scratch-open "*packages*"))
          (editor-scratch-set-prefix package-list-buffer-id "packages"))
        '())
    (editor-switch-to-buffer package-list-buffer-id)
    (package--list-render)
    (editor-goto-line 2 0)))

(define (package-list-git-status)
  (let ((name (package--target "Package: ")))
    (cond ((not name) #f)
          ((not (package--exists? (string-append (package--dir name) "/.git")))
           (if (package--link? name)
               (if (package-loaded? "git") (git-status-dir (package--dir name)) (package--say "Load the git package for this"))
               (package--say (string-append name " isn't under git: nothing to show"))))
          ((package-loaded? "git") (git-status-dir (package--dir name)))
          (else (package--say "Load the git package for this: (add-package \"git\")")))))

(define (package-list-manifest)
  (let ((name (package--target "Package: ")))
    (if name
        (let ((m (package--manifest name)))
          (package--say
           (if (null? m)
               (string-append name ": no package.lisp")
               (string-append name " " (or (package--field-string m 'version) "")
                              (let ((d (package--field-string m 'description))) (if d (string-append " — " d) ""))
                              (let ((r (package--field m 'requires)))
                                (if (null? r) ""
                                    (string-append "; requires "
                                                   (package--join (map1 (lambda (q) (if (pair? (cdr q))
                                                                                        (string-append (car q) " " (second q))
                                                                                        (car q)))
                                                                        r)
                                                                  ", "))))))))
        #f)))

(define (package-list-quit)
  (if (>= package-list-buffer-id 0) (editor-kill-buffer package-list-buffer-id) '()))

(add-hook "buffer-killed"
          (lambda (name id)
            (cond ((= id package-list-buffer-id) (set! package-list-buffer-id -1))
                  ((= id package-update-buffer-id) (set! package-update-buffer-id -1))
                  (else '()))))

;; ─── keys ───────────────────────────────────────────────────────────────────

(map-key "l" package-list "config")   ; CTRL-x c l

(add-prefix "packages" #t)
(map-key "n" '(move-cursor "down") "packages")
(map-key "p" '(move-cursor "up") "packages")
(map-key "ARROW-DOWN" '(move-cursor "down") "packages")
(map-key "ARROW-UP" '(move-cursor "up") "packages")
(map-key "ENTER" package-list-git-status "packages")
(map-key "i" package-install "packages")
(map-key "u" package-update "packages")
(map-key "U" package-update-all "packages")
(map-key "f" package-check-updates "packages")
(map-key "r" package-rollback "packages")
(map-key "d" package-remove "packages")
(map-key "s" package-sync "packages")
(map-key "/" package-search "packages")
(map-key "?" package-list-manifest "packages")
(map-key "g" package--list-refresh "packages")
(map-key "q" package-list-quit "packages")
(map-key "CTRL-x" '(editor-set-prefix "user") "packages")
(map-key "ESCAPE" '(editor-set-prefix "no-prefix") "packages")

(add-prefix "package-update" #t)
(map-key "n" '(move-cursor "down") "package-update")
(map-key "p" '(move-cursor "up") "package-update")
(map-key "ARROW-DOWN" '(move-cursor "down") "package-update")
(map-key "ARROW-UP" '(move-cursor "up") "package-update")
(map-key "PAGE-DOWN" move-cursor-page-down "package-update")
(map-key "PAGE-UP" move-cursor-page-up "package-update")
(map-key "q" package--close-update-buffer "package-update")
(map-key "CTRL-x" '(editor-set-prefix "user") "package-update")
