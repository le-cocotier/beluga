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

- [ ] **[bug]** Saving — `src/file_io.c`, `editorSave()` *(was "TODO: utf8")*
  - The UTF-8 part is done: rows go through `utf8ToString()`. What's left:
  - When `fopen()` fails, it still shows **"File saved"**. There's no error
    message, `string_path` leaks, and the buffer stays dirty.
  - The write isn't atomic. A crash or full disk halfway through leaves a
    truncated file. Write to `<file>.tmp`, `fsync`, then `rename()`.
  - The `fwrite("\n")` result is unchecked.
  - The Save-as prompt fills in `buffer->filename`, but the write uses
    `buffer->fullname`. Check that a buffer without a file can reach this
    path, and set `fullname` from the prompt if so.
  - The file's original permissions aren't kept (`fopen` recreates it
    as 0644 minus the umask).

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
- [x] **[bug]** `map-key` (`src/builtins.c`, `mapKey()`) copied the key
  with `strncpy(dst, src, 50)`, leaving 50+ character sequences
  unterminated. Now bounded at 255 and terminated. (Re-binding a key also
  overwrote the wrong binding; fixed too.)
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
- [ ] **[feature]** `add-package` of a package that isn't installed is a
  silent no-op, and so is a Lisp error inside a package. Show a status-bar
  message (and write to the debug log) naming the package and the error.
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
