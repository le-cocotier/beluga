# Beluga

Beluga is a project of CLI text editor that uses lisp as configuration language.

## Requirements

You will only need **meson** and a **C compiler** to compile the editor.

## Installation
### From source
Here is the installation line for development version:

```git clone https://git.giorgio-nas.fr/arthur/beluga.git && cd beluga && meson setup build && meson compile -C build```

The executable file will be `build/beluga`. Feel free to add it to your path.

### From installation script ( prefered )

Just clone the repo and execute the script `install.sh`. It will automatically add beluga to your path.
Run `./install.sh --help` to see its options (custom install location, non-interactive mode, skipping packages, debug builds, ...).

### Windows

Beluga runs natively on Windows 10 (1809 or later) and 11, in Windows Terminal or any
Windows console — no WSL needed.

The simplest way: download `beluga-<version>-windows-x64.zip` from the
[Releases](https://git.giorgio-nas.fr/arthur/beluga/releases) page, unzip it, and run
`install.ps1` in it (right-click → *Run with PowerShell*). No compiler needed; `README.txt`
in the zip has the details.

To build it yourself instead, from PowerShell in a clone of the repo:

```
powershell -ExecutionPolicy Bypass -File .\install.ps1
```

`install.ps1` is the Windows counterpart of `install.sh` (same config and package handling;
`Get-Help .\install.ps1 -Detailed` lists its options). It builds `beluga.exe`, offering to
install what's missing with `winget`: [Meson](https://mesonbuild.com) (Ninja included) and
MinGW-w64 gcc ([WinLibs](https://winlibs.com); MSYS2's UCRT64 gcc works too — MSVC isn't
supported). Already have a `beluga.exe`, e.g. from CI? `.\install.ps1 -Binary path\to\beluga.exe`
installs it without compiling anything.

Things that work differently on Windows:

- **Your files** live in `%USERPROFILE%\.beluga`, the Windows equivalent of `~/.beluga`.
- **Terminal panes** run PowerShell (`pwsh` if installed, else Windows PowerShell). Set
  `BELUGA_SHELL` to use another shell, e.g. `cmd.exe`.
- **dired's `!`** (and `shell-run` in Lisp) runs its command in PowerShell rather than `sh`.
- **Git integration** needs [Git for Windows](https://gitforwindows.org) (`winget install Git.Git`).
  Without it, project search and the file finder still work, on plain directories.
- **Language servers** installed with npm (`typescript-language-server`, `pyright-langserver`, ...)
  are found as usual: their `.cmd` launchers work.

**Publishing a Windows release** (maintainers): push a tag starting with `v`
(`git tag v3.3 && git push origin v3.3`). CI cross-compiles `beluga.exe` and attaches the zip
to a release for that tag (`.gitea/workflows/release-windows.yml`).

To cross-compile `beluga.exe` from Linux: `meson setup --cross-file cross/mingw-w64.ini build-windows && meson compile -C build-windows`
(needs the `mingw-w64` package).
Adding `--cross-file cross/wine.ini` to that setup lets `meson test -C build-windows` run the
Windows test programs under Wine.

**Checking memory** (developers): build with AddressSanitizer and UndefinedBehaviorSanitizer:
`meson setup -Db_sanitize=address,undefined -Dbuildtype=debug build-asan && meson compile -C build-asan`.
Everything outside `src/os/` is the same code on every platform, so Linux or macOS catches
most problems. For the Windows layer, do the same from an MSYS2 **CLANG64** shell (gcc has no
AddressSanitizer on Windows), and run `build-asan\beluga.exe` with `C:\msys64\clang64\bin` on
`PATH` (the sanitizer runtime is a DLL). Dr. Memory doesn't start on Windows 11 24H2.
Leaks: every `galloc` is tracked. `CTRL-x m` (memory inspector) lists the live blocks with
the file and line that allocated them, and a debug build (`install.sh --debug`,
`install.ps1 -DebugBuild`) writes what is still allocated at exit to
`~/.beluga/memory-at-exit.log`, grouped by allocation site.

## Packages

Everything beyond the core editor is a package: a folder with an `init.lisp`, loaded by
`(add-package "name")`. The bundled ones (`packages/` here) are installed by `install.sh` /
`install.ps1` and versioned with the editor. For other packages, declare them in
`~/.beluga/packages/personal-config/init.lisp`:

```lisp
(use-package "rainbow" "https://example.org/rainbow.git")           ; default branch
(use-package "rainbow" "https://example.org/rainbow.git" "v1.2.0")  ; a tag, branch or commit
(use-package "rainbow" "https://example.org/rainbow.git" "latest")  ; the highest version tag
(use-package "mine" "~/code/beluga-mine")                           ; a folder, loaded in place
```

A declared package that's missing is cloned when beluga starts, then loaded in the order the
lines are written. The commit each one is at goes to `personal-config/packages.lock`; with that
folder on another machine, `beluga --sync-packages` installs exactly the same commits without
opening the editor. `CTRL-x c l` lists every package (version, commit, loaded or not) and
installs (`i`), updates after showing the new commits (`u`, `U` for all), rolls back (`r`) and
removes (`d`) them. Updates apply at the next start.

A package can describe itself in a `package.lisp` next to its `init.lisp` (read, never run):

```lisp
(define-package "git-blame"
  (version "1.2.0")
  (description "Blame annotations in the gutter")
  (requires ("beluga" "3.4") ("git" "3.3"))
  (sources ("git" "https://example.org/beluga-git.git")))
```

`add-package` then loads what it requires first and refuses it on an older beluga, and
installing it installs its missing requirements. `(define package-index-url "...")` points at a
git repository whose `index.lisp` lists packages by name, for `use-package` without a URL and
`M-x package-search`. All the details are at the top of
[`packages/package/init.lisp`](packages/package/init.lisp).

`BELUGA_HOME` moves `~/.beluga` elsewhere (tests, a portable copy). `meson test` runs the
package manager end to end against throwaway repositories (`test/test_packages.sh`).

## How beluga talks to the operating system

Everything beluga needs from the OS — the terminal, waiting for input, running programs,
terminal panes, files and directories, the clipboard, time — is declared in
[`include/os.h`](include/os.h) and implemented by one of two small libraries:

| Library               | Source                 | For                       |
|-----------------------|------------------------|---------------------------|
| `libbeluga-os-posix`  | `src/os/os_posix.c`    | Linux, macOS, BSD         |
| `libbeluga-os-win32`  | `src/os/os_win32.c`    | Windows 10 (1809+) and 11 |

The rest of the editor is plain C99 plus `os.h`: no `<unistd.h>`, no `<windows.h>`, no
`#ifdef _WIN32`. That's checked, not just intended — the editor is compiled without any
feature-test macros and with `-Werror=implicit-function-declaration`, so calling a POSIX or
Windows function outside `src/os/` doesn't build, and `meson test` runs
`tools/check_os_boundary.sh` to catch OS headers and platform `#ifdef`s.
It also runs the unit tests in `test/` (UTF-8 helpers, buffer edits with undo/redo, the
terminal output parser), which link the editor's code without `main.c`.

As a result, behaviour is the same everywhere:

- Programs are always started from an argument list, never a shell string: git, language
  servers, `process-run`. Only `shell-run` (dired's `!`) goes through a shell, on purpose.
- dired's delete, rename, copy and mkdir are the editor's own file operations, not `rm`, `mv`,
  `cp` and `mkdir`.
- Project search and the file finder use git inside a repository, and their own directory walk
  and search elsewhere — no `find` or `grep` needed.
- Files using CRLF line endings are edited without stray `^M`s and saved back as CRLF; new
  files are saved with LF.

Porting beluga to another system means writing one more `os_*.c` against `os.h` (and a line in
`meson.build` to pick it).

## Getting start

To open an existing file just type :
```./build/beluga path_to_my_file```

Here is some few command that you will need :

| keybind       | command          |
|---------------|------------------|
| Ctrl-x Ctrl-c | leave the editor |
| Ctrl-x Ctrl-s | Save a file      |
| Ctrl-x f      | open file        |

### Daemon mode: several windows on one editor

```
beluga --daemon          # start the daemon in the background (--foreground: in this terminal)
beluga -c [file]         # open a window on it, from any terminal (starts one if none runs)
beluga --kill            # stop it (refused while a buffer is unsaved; --force to insist)
```

A daemon in the background writes what it has to say to `~/.beluga/daemon-server.log`
(`daemon-NAME.log` for a named one). One in the foreground stops on Ctrl-C, but asks
first when a buffer is unsaved: a second Ctrl-C within a few seconds stops it anyway. Closing
its terminal, or SIGTERM, stops it at once. Either way the unsaved changes are kept in
`~/.beluga/autosave` and offered back when the file is opened again.

Every window shares the daemon's buffers, Lisp config, language servers and terminal panes,
and has its own splits, popups, status line and working directory (where its `beluga -c` ran:
the open-file prompt and new terminal panes start there). Each project has its own debug
session, so two projects can be debugged at once; a session's stops and output go to the window
that last drove it. Ctrl-x Ctrl-c in a window closes only that window. Several daemons can run side by side: `--daemon=work`, then `beluga -c --name=work`
and `beluga --kill=work`. On Unix the windows reach the daemon through a socket in a private
directory (`$XDG_RUNTIME_DIR/beluga/`, else `/tmp/beluga-<uid>/`); on Windows through a named
pipe only the current user can open.

### Debugging

Beluga talks the Debug Adapter Protocol, so it debugs with whatever adapter a language has:
`gdb` 14 or later for C (`gdb -i dap`), `lldb-dap` on macOS, `debugpy` for Python. Each
language package names its adapter, like it names its language server:

```lisp
(dap-adapter "c" "gdb" "-q" "-i" "dap")   ; packages/c does this (xcrun lldb-dap on macOS)
```

An adapter that talks over TCP rather than stdio gets `{port}` where it is told which port to
listen on; beluga picks a free one and connects to it. One that is already running is named by
its address instead of a command:

```lisp
(dap-adapter "go" "dlv" "dap" "--listen=127.0.0.1:{port}")
(dap-adapter "js" "tcp:127.0.0.1:8123")
```

An association list after the command gives launch arguments every session gets unless it sets
them itself; on macOS packages/c uses it for lldb's `sourceMap`, so breakpoints in files under
`/tmp` or `/var` (links into `/private`) are found.

Everything is under Ctrl-x B, and inside the `*debug*` panel the same keys work without the
prefix (c, n, i, u, q, p, e, g, o). There are no function keys by default (on a Mac the F-row
is media keys, and F11 is Show Desktop); bind them in personal-config if you like,
`(map-key "F5" debug-continue "no-prefix")`.

| keybind      | command                                                  |
|--------------|----------------------------------------------------------|
| Ctrl-x B b   | toggle a breakpoint on the cursor's line (Ctrl-x B k: with a condition) |
| Ctrl-x B h   | stop there only from a given hit on (`3`; gdb also takes `> 5`, `% 2`) |
| Ctrl-x B L   | a logpoint: log `i is {i}` there instead of stopping     |
| Ctrl-x B E   | which exceptions stop the program (the adapter's filters: debugpy's `raised`, gdb's `throw`) |
| Ctrl-x B c   | start (asks for the program and its arguments), or continue |
| Ctrl-x B a   | attach to a running program, picked from your processes (Ctrl-x B q detaches) |
| Ctrl-x B n / i | next line / step into                                  |
| Ctrl-x B u   | step out                                                 |
| Ctrl-x B p   | pause                                                    |
| Ctrl-x B q   | stop                                                     |
| Ctrl-x B e   | evaluate an expression in the selected frame             |
| Ctrl-x B w / W | watch an expression (its value is in the panel at every stop) / stop watching it |
| Ctrl-x B g   | a command for the debugger itself (gdb's `info registers`, lldb's `bt`) |
| Ctrl-x B o   | everything the program printed                           |
| Ctrl-x B l / x | list the breakpoints / remove them all                 |

While the program is paused, the `*debug*` panel shows the call stack, the watch expressions,
the variables (ENTER selects a frame or opens a struct) and the end of the output, and the lines
up to the current one show the values of the variables they use (`(define DEBUG-INLINE-VALUES
#f)` hides them); the gutter shows `▶` on the
current line, `●` on breakpoints, `◆` on those with a condition or a hit count and `◉` on
logpoints, which follow the code as you edit. Breakpoints are kept
in `~/.beluga/breakpoints` from one run to the next, and a session only gets those in its own
project (the files under its root, or under the working directory). For anything the
Ctrl-x B c prompt doesn't cover, hand the adapter its own launch arguments, e.g. in a project's
`.beluga/init.lisp`:

```lisp
(define (debug-tests)
  (debug-start '(("program" . "build/test_buffer") ("args" "-v")
                 ("stopAtBeginningOfMainSubprogram" . #t))))
```

A program that reads the keyboard or draws on the terminal (curses) can run in a terminal pane
of its own, `*debuggee*`, which takes the keys (ESC gives them back to the editor): ask the
adapter for it, with `("console" . "integratedTerminal")` for debugpy or `("runInTerminal" . #t)`
for lldb-dap. gdb has no such option; its program's output arrives in the panel.

The hooks `debug-stopped` (path line reason) and `debug-exited` (exit-code) let a config react,
and `debug-frames`, `debug-breakpoints`, `debug-watches` and `debug-state` read the session from Lisp.
