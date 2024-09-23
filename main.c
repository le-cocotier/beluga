/**
 * \file main.c
 * \author Arthur Barraux
 * \brief Base file of Beluga text editor. Contain fonctions for terminal
 * interactions. \version 0.1 \date 21 septembre 2024
 */

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include "include/data.h"
#include "include/file_io.h"
#include "include/init.h"
#include "include/input.h"
#include "include/output.h"
#include "include/terminal.h"

int main(int argc, char *argv[]) {

  struct editorConfig E;

  enableRawMode(&E);
  initEditor(&E);
  if (argc >= 2) {
    editorOpen(&E, argv[1]);
  }

  while (1) {
    editorRefreshScreen(&E);
    editorProcessKeypress(&E);
  }
  return 0;
}
