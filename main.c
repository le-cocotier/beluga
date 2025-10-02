/**
 * \file main.c
 * \author Arthur Barraux
 * \brief Base file of Beluga text editor. Contain fonctions for terminal
 * interactions. \version 0.1 \date 21 septembre 2024
 */

#include <stdio.h>
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include "include/data.h"
#include "include/file_io.h"
#include "include/init.h"
#include "include/input.h"
#include "include/output.h"
#include "include/terminal.h"

struct editorConfig E;

int main(int argc, char *argv[]) {

  enableRawMode();
  initEditor();
  if (argc >= 2) {
    editorOpen(argv[1]);
  }

  editorSetStatusMessage("HELP: Ctrl-S = save | Ctrl-Q = quit");

  while (1) {
    editorRefreshScreen();
    editorProcessKeypress();
  }
  return 0;
}
