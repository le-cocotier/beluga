/**
 * \file main.c
 * \author Arthur Barraux
 * \brief Base file of Beluga text editor. Contain functions for terminal
 * interactions. \version 0.1 \date 21 septembre 2024
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "include/buffer.h"
#include "include/builtins.h"
#include "include/data.h"
#include "include/define.h"
#include "include/editor_op.h"
#include "include/galloc.h"
#include "include/init.h"
#include "include/os.h"
#include "include/split_screen.h"
#include "include/terminal.h"
#include "include/utf8.h"

EditorConfig E;
int buffer_id = 0;

#ifdef APP_DEBUG
// Debug builds: whatever galloc still tracks once everything has been torn
// down is a leak (or something deliberately kept to the end). Written to
// ~/.beluga/memory-at-exit.log, with a one-line summary on the terminal.
static void reportMemoryAtExit(void) {
  char path[OS_PATH_MAX];
  FILE *out;
  int left;
  snprintf(path, sizeof(path), "%s/.beluga/memory-at-exit.log", os_home_dir());
  out = fopen(path, "w");
  if (!out)
    return;
  left = gReportMemory(out);
  fclose(out);
  if (left > 0)
    printf("beluga (debug): %d blocks still allocated at exit, see %s\n", left,
           path);
}
#endif

int main(const int argc, char *argv[]) {
  const char *home;
  Utf8Char *splash_screen;
  EditorPane *active;
  Utf8Char *filename;
  Utf8Char *config_msg;
  if (argc >= 2 && (!strcmp(argv[1], "--version") || !strcmp(argv[1], "-v"))) {
    printf("Beluga %s\n", BELUGA_VERSION);
    return 0;
  }

  // SIGPIPE ignored on Unix (a write to a dead shell or language server
  // must not kill the editor); binary stdio and a UTF-8 console on Windows.
  os_init();

  home = os_home_dir();
  splash_screen = gcalloc(PATH_MAX, sizeof(Utf8Char));
  active = NULL;
  utf8Printf(splash_screen, PATH_MAX, "%s/.beluga/assets/beluga.txt", home);

  enableRawMode();
  initEditor();
  // A message init.lisp left (a package that couldn't load, a failed
  // use-package, ...) is worth more than the help line below: keep it.
  config_msg = NULL;
  if (*E.status_msg[0].character) {
    config_msg = gcalloc(PATH_MAX, sizeof(Utf8Char));
    utf8Cpy(config_msg, E.status_msg);
  }

  active = splitScreenGetActivePane();
  active->buffer_id = bufferCreate(splash_screen, READ_ONLY, FILE_BUFF);

  if (argc >= 2) {
    filename = stringToUtf8((unsigned char *)argv[1]);
    active->buffer_id = bufferCreate(filename, READ_AND_WRITE, FILE_BUFF);
    gfree(filename);
  }

  gfree(splash_screen); // Now guaranteed safe
  if (config_msg) {
    editorSetStatusMessage("%u", config_msg);
    gfree(config_msg);
  } else {
    editorSetStatusMessage("HELP: Ctrl-x Ctrl-s = save | Ctrl-x Ctrl-c = quit");
  }

  while (!E.exit_requested) {
    editorRunLoop();
  }

  // Only reached once editorRunLoop() has actually returned — no Lisp
  // evaluation is on the stack at this point, so it's safe to tear down
  // the interpreter here (editorQuit() used to do this while still
  // inside a live lisp_apply call, which was a use-after-free).
  deInitEditor();
  bFree_structs();
#ifdef APP_DEBUG
  reportMemoryAtExit();
#endif
  return 0;
}
