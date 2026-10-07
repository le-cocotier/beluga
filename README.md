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
Windows console — no WSL needed. From PowerShell, in a clone of the repo:

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
- **Git integration, project search and the file finder** use [Git for Windows](https://gitforwindows.org)
  (`winget install Git.Git`). Project search outside a repository uses `git grep --no-index`
  instead of `grep`.
- **dired**'s delete, rename, copy and mkdir use Windows' own file operations, and `!` runs its
  command in PowerShell.
- **Language servers** installed with npm (`typescript-language-server`, `pyright-langserver`, ...)
  are found as usual: their `.cmd` launchers work.
- **Line endings:** files using CRLF are edited without stray `^M`s and saved back as CRLF
  (on every platform); new files are saved with LF.

To cross-compile `beluga.exe` from Linux: `meson setup --cross-file cross/mingw-w64.ini build-windows && meson compile -C build-windows`
(needs the `mingw-w64` package).

## Getting start

To open an existing file just type :
```./build/beluga path_to_my_file```

Here is some few command that you will need :

| keybind       | command          |
|---------------|------------------|
| Ctrl-x Ctrl-c | leave the editor |
| Ctrl-x Ctrl-s | Save a file      |
| Ctrl-x f      | open file        |
