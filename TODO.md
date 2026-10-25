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

- [ ] **[bug]** Typing fast into a terminal pane drops keystrokes (seen on
  macOS). No repro yet: in a scripted pty every key arrives on Linux (bash,
  dash, zsh) and on macOS (zsh with oh-my-zsh + powerlevel10k, bash), in
  bursts up to 2000 bytes, keys 0-5 ms apart, during `sleep` and
  `seq 1 200000`, ASCII, control keys and UTF-8. The `pty` meson test
  (`terminal-fast-typing`, `terminal-long-burst`) keeps checking it.
  Two things lose input on macOS and look like it:
  - A canonical-mode line (into `cat`, or typed while a command runs)
    longer than 1024 bytes is discarded by the macOS tty itself.
  - CTRL-g or ESC take the pane out of the `term` keymap (the status line
    shows "no-prefix"): letters still reach the shell, but ENTER, TAB,
    BACKSPACE, CTRL-a/e/k run editor commands that ignore a terminal.
  Next step: a repro in the real terminal app (which app, what was typed,
  what the status line's prefix showed). Start from `ptyForwardKey()`
  (`src/input.c`) and `editorRunLoop()` (`src/editor_op.c`).

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
