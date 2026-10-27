# TODO

One list for everything that's still open. Code comments that used to carry
a `TODO:` now point here (`TODO(TODO.md): …`) so the detail lives in one
place. Locations are given as file + function rather than line numbers, which
drift.

Legend: **[bug]** wrong behaviour today · **[debt]** works, but fragile or
incomplete · **[feature]** new capability.

---

## Daemon mode

`beluga --daemon` / `beluga -c` (src/daemon.c, src/daemon_client.c). What's
left after the first step:

- [ ] **[debt]** Prompts still nest on one stack: the other windows are
  served while a window waits in its `editorPrompt()`, but when a second
  window opens a prompt meanwhile, the first one's keys wait until the
  second prompt closes. Making the prompt a per-window state driven by the
  run loop (its ~35 callers continuing in a callback) would lift that.

---

## Debugger

The Debug Adapter Protocol client (src/dap.c, src/debug_ui.c,
src/breakpoints.c). What's left after the first step:

- [ ] **[debt]** Not tried yet with lldb-dap on Windows. gdb 15 on Linux,
  lldb-dap on macOS and debugpy (`runInTerminal` included) are covered by
  `tools/pty_tests.py`.
