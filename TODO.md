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

- [ ] **[debt]** Prompts block: while one window has an `editorPrompt()`
  open (open-file, M-x, search, kill-buffer confirmation), the other windows
  don't redraw or take keys until it closes. Make the prompt a per-window
  state driven by the run loop instead of a nested key loop.

---

## Debugger

The Debug Adapter Protocol client (src/dap.c, src/debug_ui.c,
src/breakpoints.c). What's left after the first step:

- [ ] **[debt]** Not tried yet with lldb-dap on Windows. gdb 15 on Linux,
  lldb-dap on macOS and debugpy (`runInTerminal` included) are covered by
  `tools/pty_tests.py`.

---

## Tooling and housekeeping

Nothing open right now.
