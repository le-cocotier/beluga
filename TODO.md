# TODO

One list for everything that's still open. Code comments that used to carry
a `TODO:` now point here (`TODO(TODO.md): …`) so the detail lives in one
place. Locations are given as file + function rather than line numbers, which
drift.

Legend: **[bug]** wrong behaviour today · **[debt]** works, but fragile or
incomplete · **[feature]** new capability.

---

## From the code

Nothing open right now.

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
  - `tools/pty_tests.py` (`meson test`, the `pty` test) now types into a
    terminal pane in one burst and 2 ms apart, so the Linux case is
    checked on every run.
- [ ] **[feature]** `ESCAPE` in a terminal pane always goes back to the
  editor (`config/init.lisp`, the `term` prefix), so it never reaches a
  program: in vim, only CTRL-c leaves insert mode (CTRL-[ is the same
  byte as `ESCAPE`, so the pane takes it too). Options:
  send `ESCAPE` to the program while it's on the alternate screen (vim,
  less, htop) and use another key to leave the pane there, or a double
  `ESCAPE`. A choice for the keymap, not a parser problem.
- [ ] **[feature]** The terminal parser doesn't wrap: it doesn't know the
  pane's width, so a line longer than the pane stays one row and is cut at
  the pane's edge (`src/ansi_parser.c`, `putChar()`). Wrapping needs the
  width in `TermParserState` (set where `pane_height` is) and xterm's
  pending-wrap rule (the cursor stays on the last column until the next
  character).

---

## Configuration and packages

Nothing open right now.

---

## Daemon mode

`beluga --daemon` / `beluga -c` (src/daemon.c, src/daemon_client.c). What's
left after the first step:

- [ ] **[feature]** A cursor per pane instead of per buffer
  (`struct buffer_t` x/y): two windows, or two split panes, on the same file
  share one cursor today.
- [ ] **[feature]** `beluga -c` starts a daemon when none is running, and
  `--daemon` detaches from its terminal (fork on Unix, a detached process on
  Windows). Today the daemon stays in the foreground.
- [ ] **[debt]** Prompts block: while one window has an `editorPrompt()`
  open (open-file, M-x, search, kill-buffer confirmation), the other windows
  don't redraw or take keys until it closes. Make the prompt a per-window
  state driven by the run loop instead of a nested key loop.
- [ ] **[debt]** A per-window working directory. The client sends its own
  in the hello (used for the files on its command line), but the open-file
  prompt, find-file, project search and new terminal panes still use the
  daemon's (`os_getcwd()`).
- [ ] **[debt]** Ctrl-C (or a closed terminal) on the daemon kills it
  without asking about unsaved buffers; only `beluga --kill` checks. The
  stale socket left on Unix is replaced at the next start.
- [ ] **[debt]** which-key's pending-prefix timer (`whichKeyDraw()`'s
  statics) is shared between windows.

---

## Tooling and housekeeping

Nothing open right now.
