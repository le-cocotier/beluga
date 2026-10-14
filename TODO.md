# TODO

One list for everything that's still open. Code comments that used to carry
a `TODO:` now point here (`TODO(TODO.md): …`) so the detail lives in one
place. Locations are given as file + function rather than line numbers, which
drift.

Legend: **[bug]** wrong behaviour today · **[debt]** works, but fragile or
incomplete · **[feature]** new capability.

---

## From the code

These are the `TODO:` comments that were in the sources, restated with what
is actually left to do.

- [ ] **[bug]** Tab path completion in prompts — `src/input.c`, `fileCompletion()` / `editorPrompt()` *(was "TODO: upgrade this")*
  - The loop skips the first two `readdir()` entries on the assumption that
    they're `.` and `..`. POSIX doesn't guarantee that order (ext4 with
    hashed directories, network filesystems), so a real entry can vanish
    from completion. Skip by name instead.
  - The returned `full_path` is never freed by `editorPrompt()`: one leak per
    TAB. The `opendir()`-failed path also leaks it (it returns the caller's
    `path` instead).
  - Only the first match is used: no common-prefix completion, no cycling
    through candidates on repeated TAB, no listing of the matches.

- [x] ~~Saving~~ — `src/file_io.c`, `editorSave()`: a failed open or write
  is reported and leaves the buffer modified; the file is written to a
  temporary file, flushed to disk and renamed over the original
  (`os_replace_file()`), keeping its permissions. Still open: the Save-as
  prompt fills in `buffer->filename` while the write uses
  `buffer->fullname` (no buffer seems to reach it without a file today).

- [ ] **[debt]** Cursor movement and wide characters — `src/editor_op.c`, `editorMoveCursor()` *(was "TODO: Rebuild for utf8")*
  - Movement already steps one `Utf8Char` at a time, so the rebuild itself
    is done. What's missing is **display width**. CJK characters and most
    emoji take 2 terminal columns, and combining marks take 0. Nothing in
    the renderer uses `wcwidth()`, so the cursor and everything to its right
    drift on those lines. This needs a column-width helper, used by the
    cursor, the renderer and the selection highlight.

- [ ] **[debt]** Paste — `src/builtins.c`, `editorPaste()` *(was "TODO: Rebuild pasting functionality", above `editorGetClipboard()`)*
  - Paste works. It inserts the clipboard one character at a time through
    `bufferInsertChar()`. What's left:
  - `\r` from CRLF clipboards is inserted as a literal character.
  - Every character goes through the full insert path (dirty flag, highlight
    invalidation, auto-behaviours). Measure a large paste, and add a bulk
    insert if it's slow.
  - Check that the whole paste undoes as one step.

---

## Known bugs (not marked in the code)

- [ ] **[bug]** Typing fast into a terminal pane drops keystrokes. This
  reproduces on older binaries too, so it's not a regression. Start from
  `ptyForwardKey()` (`src/input.c`) and the input read in
  `editorRunLoop()` (`src/editor_op.c`).
- [ ] **[bug]** `map-key` (`src/builtins.c`, `mapKey()`) copies the key
  with `strncpy(dst, src, 50)` into a `galloc(256)` buffer that isn't
  zeroed. A key sequence of 50+ characters has no terminator. Copy with a
  bound of `255` and terminate it explicitly, or `strdup` it.
- [ ] **[bug]** ANSI parser gaps in the terminal buffer (`src/ansi_parser.c`):
  - `ESC[1K` (erase from start of line to cursor) is ignored.
  - Unknown CSI sequences and C0 control characters are silently dropped.
  - TAB inserts 4 spaces instead of moving to the next 8-column tab stop,
    which misaligns `ls` columns and similar output.
  - Full-screen programs (`htop`, `vim`, `less`) will show more of these.

---

## Configuration and packages

  Replacing the existing key+prefix entry fixes both. It flips the load
  order rule, though: `(add-package "personal-config")` would then have to
  move to the end of `config/init.lisp`.
- [ ] **[feature]** Per-language (buffer-local) keymaps, so the comment toggle
  and similar commands pick the right behaviour for the file type instead of
  whichever package loaded first.
- [ ] **[debt]** `nodejs` and `angular` both register the `ts` extension, and
  only the first one wins. Detect the conflict and say so in the debug log
  instead of silently ignoring the second one.
- [x] ~~`add-package` errors~~: a missing package, and any error in
  `init.lisp` or a package, is reported with its file and line (status bar
  plus a `*config-errors*` buffer); the rest of the file still loads
  (`src/config_load.c`).
- [ ] **[feature]** An `after-init` hook, fired once all of `init.lisp` has
  loaded. `personal-config` loads before the helper functions defined further
  down the default config (`enter-and-tab`, `editor-buffer-picker`, …), so it
  can't bind them today.
- [x] ~~`install.sh --update`~~: bundled packages now live in `packages/` and
  every install brings them up to date (`--dev` symlinks them instead).
- [ ] **[feature]** `use-package`: `(use-package "name" ["git-url"])`, which
  clones a missing non-bundled package on first use and reports a missing
  or failing one in the status bar (see the `add-package` item above).
- [x] ~~Git manager~~: the `git` package (`CTRL-x g`) — status, stage,
  discard, diff, commit/amend, log, branches, and push/pull/fetch in the
  background through the new `process-start`.
- [ ] **[feature]** A `*packages*` buffer listing every package (bundled /
  git / local, loaded?, branch, dirty, ahead/behind), opening the `*git*`
  view (`git-status-dir`) on a package's repository.
- [ ] **[feature]** git package: multi-line commit messages (today's prompt
  is one line), hunk-level staging, colored diffs (the highlighter is
  keyword-based and can't color `+`/`-` lines yet).

---

## Tooling and housekeeping

- [ ] **[debt]** Commit the pty regression harness (pyte, openpty sized
  before fork, SIGALRM watchdog) as a `meson test` target. For now it only
  exists as throwaway scripts, and `test/` is gitignored.
- [ ] **[debt]** Unused `#include`s flagged by the CLion analysis haven't
  been verified as removed. Re-run the inspection (or `include-what-you-use`
  / clangd's unused-include check) and clean up what's left.
- [ ] **[debt]** `Doxyfile` has `GENERATE_TODOLIST = YES`, but nothing uses
  `\todo`. Either keep this file as the only list and turn that off, or tag
  the code and let Doxygen build the list.
