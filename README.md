# Beluga

Beluga is a project of CLI text editor that uses lisp as configuration language.

## Requirements

You will only need **meson** and a **C compiler** to compile the editor.

## Installation
### From source
Here is the installation line for development version:

```git clone https://git.giorgio-nas.fr/beluga/beluga.git && cd beluga && meson setup build && meson compile -C build```

The executable file will be `build/beluga`. Feel free to add it to your path.

### From installation script ( prefered )

Just clone the repo and execute the script `install.sh`. It will automatically add beluga to your path.
Run `./install.sh --help` to see its options (custom install location, non-interactive mode, skipping packages, debug builds, ...).

## Getting start

To open an existing file just type :
```./build/beluga path_to_my_file```

Here is some few command that you will need :

| keybind       | command          |
|---------------|------------------|
| Ctrl-x Ctrl-c | leave the editor |
| Ctrl-x Ctrl-s | Save a file      |
| Ctrl-x f      | open file        |
