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
  - The write isn't atomic. A crash or full disk halfway through leaves a
    truncated file. Write to `<file>.tmp`, `fsync`, then `rename()`.
  - The `fwrite("\n")` result is unchecked.
  - The Save-as prompt fills in `buffer->filename`, but the write uses
    `buffer->fullname`. Check that a buffer without a file can reach this
    path, and set `fullname` from the prompt if so.
  - The file's original permissions aren't kept (`fopen` recreates it
    as 0644 minus the umask).

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
  - Doesn't reproduce on Linux (bash and dash, a 100-character line sent in
    one write, keys 2 ms apart, typing during `seq 1 200000`): every key
    arrived. Input isn't buffered in user space (`readKeyBytes()` reads
    straight from the fd), so a key can't be lost between ticks. Likely
    macOS-specific; next step is a repro there (which shell, which
    terminal app, what was typed).
- [ ] **[feature]** Full-screen programs in the terminal buffer
  (`src/ansi_parser.c`): `htop`, `vim` and `less` need scroll regions
  (`ESC[r`), the alternate screen (`ESC[?1049h`), insert/delete line
  (`ESC[L` / `ESC[M`) and 256-color/truecolor SGR (`38;5;n`, `38;2;r;g;b`).
  The parser's cursor also counts characters, not columns, so a shell
  positioning the cursor on a line with wide characters lands off by one
  per wide character (file buffers are column-aware).
  Today DEC private sequences are recognised and skipped, and other unknown
  final bytes are ignored.

---

## Configuration and packages

- [ ] **[feature]** An `after-init` hook, fired once all of `init.lisp` has
  loaded. `personal-config` now loads last, so it can already bind the
  helpers defined in the default config; the hook itself is on
  `feature/after-init-hook`.

---

## Tooling and housekeeping

- [ ] **[debt]** Commit the pty regression harness (pyte, openpty sized
  before fork, SIGALRM watchdog) as a `meson test` target. For now it only
  exists as throwaway scripts, and `test/` is gitignored.
