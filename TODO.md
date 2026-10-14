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

- [ ] **[feature]** Tab path completion in prompts — `src/input.c`, `fileCompletion()` / `editorPrompt()` *(was "TODO: upgrade this")*
  - TAB completes the common prefix and a repeated TAB cycles through the
    matches. What's left: showing the list of matches while cycling.

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
    `bufferInsertChar()`. What's left (both done on `feature/range-undo`):
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
- [ ] **[feature]** Full-screen programs in the terminal buffer
  (`src/ansi_parser.c`): `htop`, `vim` and `less` need scroll regions
  (`ESC[r`), the alternate screen (`ESC[?1049h`), insert/delete line
  (`ESC[L` / `ESC[M`) and 256-color/truecolor SGR (`38;5;n`, `38;2;r;g;b`).
  Today DEC private sequences are recognised and skipped, and other unknown
  final bytes are ignored.

---

## Configuration and packages

- [ ] **[feature]** Let a later `map-key` replace an existing binding.
  Bindings are appended and the **first** registered one wins, so:
  - a key taken by a package can't be rebound from `personal-config` or at
    runtime;
  - every language package fights over `CTRL-d` (see the note in
    `config/init.lisp`).

  Replacing the existing key+prefix entry fixes both. It flips the load
  order rule, though: `(add-package "personal-config")` would then have to
  move to the end of `config/init.lisp`.
- [ ] **[feature]** Per-language (buffer-local) keymaps, so the comment toggle
  and similar commands pick the right behaviour for the file type instead of
  whichever package loaded first.
- [ ] **[feature]** `add-package` of a package that isn't installed is a
  silent no-op, and so is a Lisp error inside a package. Show a status-bar
  message (and write to the debug log) naming the package and the error.
- [ ] **[feature]** An `after-init` hook, fired once all of `init.lisp` has
  loaded. `personal-config` loads before the helper functions defined further
  down the default config (`enter-and-tab`, `editor-buffer-picker`, …), so it
  can't bind them today.
- [ ] **[feature]** `use-package`: `(use-package "name" ["git-url"])`, which
  clones a missing non-bundled package on first use and reports a missing
  or failing one in the status bar (see the `add-package` item above).
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
