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
