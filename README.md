# Beluga

Beluga is a project of CLI text editor that uses lisp as configuration language.
It's abviously only working for **Linux**.

## Requirements

You will only need **meson** and a **C compiler** to compile the editor.

## Installation

Here is the installation line for development version:

```git clone https://homelinuxserver.ddns.net/git/arthur/beluga.git ~/.beluga && cd ~/.beluga && meson setup --reconfigure build && meson compile -C build```

The executable file will be `build/beluga`. Feel free to add it to your path.

## Getting start

To open an existing file just type :
```./build/beluga path_to_my_file```

Here is some few command that you will need :

| keybind| command          |
|--------|------------------|
| Ctrl-Q | leave the editor |
| Ctrl-S | Save a file      |
| Ctrl-O | open file        |
