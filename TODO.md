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
  - Doesn't reproduce on macOS in a scripted pty either: zsh (bare, and the
    real oh-my-zsh + powerlevel10k config) and bash; bursts up to 2000
    bytes at the prompt; keys 0-5 ms apart; typing right after CTRL-x t;
    typeahead during `sleep 1`; typing during `seq 1 200000`; all of
    printable ASCII, every control key but CTRL-g/CTRL-x/ESC, and UTF-8
    (è à € 日). Two things that do lose input there and look like it: a
    canonical-mode line (into `cat`, or typed while a command runs) longer
    than 1024 bytes is discarded by the macOS tty itself, editor or not;
    and CTRL-g or ESC drop the pane out of the "term" keymap, after which
    ENTER, TAB, BACKSPACE, CTRL-a/e/k/... run editor commands that ignore
    a terminal buffer, while letters still reach the shell. Next step: a
    repro in the real terminal app (which one, what was typed, what the
    status line's prefix showed).
- [ ] **[feature]** `ESCAPE` in a terminal pane always goes back to the
  editor (`config/init.lisp`, the `term` prefix), so it never reaches a
  program: in vim, only CTRL-c leaves insert mode (CTRL-[ is the same
  byte as `ESCAPE`, so the pane takes it too). Options:
  send `ESCAPE` to the program while it's on the alternate screen (vim,
  less, htop) and use another key to leave the pane there, or a double
  `ESCAPE`. A choice for the keymap, not a parser problem.
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

## Debugger

The Debug Adapter Protocol client (src/dap.c, src/debug_ui.c,
src/breakpoints.c). What's left after the first step:

- [ ] **[feature]** `runInTerminal`: run the program in a terminal pane,
  for programs that read stdin or draw with curses. Today the adapter's
  request is refused and the program's output arrives as output events
  (`refuseRequest()` in `src/dap.c`).
- [ ] **[feature]** Adapters reached over TCP (delve's `dlv dap`, codelldb,
  js-debug) need a TCP connect in `os.h`; only stdio adapters work.
- [ ] **[feature]** Attach to a running process (`attach` with a pid
  picked from a list), exception breakpoints, watch expressions, values
  inline after the code while paused (the inlay hint drawing could carry
  them), logpoints and hit counts (gdb supports both).
- [ ] **[feature]** Keep breakpoints across restarts, per project. Today
  they last until beluga exits.
- [ ] **[debt]** Daemon mode: one session for the whole daemon, and the
  stopped line moves whichever window last typed. Give each project its
  own session.
- [ ] **[debt]** gdb sets the breakpoints before `launch` has loaded the
  program (it runs the program as soon as `launch` arrives, see the comment
  at the top of `src/dap.c`), so every session's output starts with "No
  source file named …" and a pending breakpoint, which then resolves.
- [ ] **[debt]** A breakpoint stays on its row when ENTER is pressed at
  the very start of that row, so it ends up on the new empty line above
  the code (`breakpointsLinesInserted()` callers in `src/buffer.c`).
- [ ] **[debt]** Not tried yet with debugpy, or lldb-dap on Windows.
  gdb 15 on Linux and lldb-dap on macOS are covered by
  `tools/pty_tests.py`.

---

## Tooling and housekeeping

Nothing open right now.
