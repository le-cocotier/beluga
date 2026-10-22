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
#include "include/config_load.h"
#include "include/data.h"
#include "include/define.h"
#include "include/editor_op.h"
#include "include/galloc.h"
#include "include/init.h"
#include "include/os.h"
#include "include/settings.h"
#include "include/split_screen.h"
#include "include/terminal.h"
#include "include/utf8.h"
#include "include/utils.h"

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
  long long heap_blocks;
  long long heap_bytes;
  snprintf(path, sizeof(path), "%s/memory-at-exit.log", utilBelugaDir());
  appDebugClose();
  out = fopen(path, "w");
  if (!out)
    return;
  left = gReportMemory(out);
  // galloc only sees its own blocks. The allocator's view covers the rest:
  // libc's (locale, stdio, this report's FILE), libraries', raw mallocs.
  if (os_heap_in_use(&heap_blocks, &heap_bytes) == 0) {
    if (heap_blocks >= 0)
      fprintf(out, "\nprocess heap: %lld blocks, %lld bytes in use", heap_blocks,
              heap_bytes);
    else
      fprintf(out, "\nprocess heap: %lld bytes in use", heap_bytes);
    fprintf(out, " (galloc's above included; the rest isn't beluga's own)\n");
  }
  fclose(out);
  if (left > 0)
    printf("beluga (debug): %d blocks still allocated at exit, see %s\n", left,
           path);
}
#endif

/**
 * @brief beluga --sync-packages: install what the config declares, without
 * opening the editor.
 * @details Loads the config as a normal start would, but with no screen
 * (E.batch): status messages are printed instead of drawn, and the package
 * manager installs missing packages in the foreground while init.lisp
 * loads. Then (package-sync) brings every declared package to its locked
 * commit and reports what it did. For a new machine, a container, or a
 * dotfiles bootstrap script.
 * @return the exit status: 0 when every package is installed and loads
 */
static int syncPackages(void) {
  int present = 0;
  int ok;
  Lisp sync;
  LispError error = LISP_ERROR_NONE;

  os_init();
  E.batch = 1;
  initEditor();
  sync = lisp_env_lookup(lisp_env(E.ctx), lisp_make_symbol("package-sync", E.ctx), &present);
  if (!present) {
    printf("beluga: the \"package\" package isn't loaded by %s, so there is nothing to sync\n",
           E.init_file_path);
    ok = 0;
  } else {
    ok = lisp_is_true(lisp_eval(lisp_cons(sync, lisp_null(), E.ctx), &error, E.ctx)) &&
         error == LISP_ERROR_NONE;
    if (error != LISP_ERROR_NONE)
      printf("beluga: package-sync failed: %s\n", lisp_error_string(error));
  }
  deInitEditor();
  bFree_structs();
#ifdef APP_DEBUG
  reportMemoryAtExit();
#endif
  return ok ? 0 : 1;
}

int main(const int argc, char *argv[]) {
  Utf8Char *splash_screen;
  EditorPane *active;
  Utf8Char *filename;
  Utf8Char *config_msg;
  if (argc >= 2 && (!strcmp(argv[1], "--version") || !strcmp(argv[1], "-v"))) {
    printf("Beluga %s\n", BELUGA_VERSION);
    return 0;
  }
  if (argc >= 2 && !strcmp(argv[1], "--sync-packages"))
    return syncPackages();

  // SIGPIPE ignored on Unix (a write to a dead shell or language server
  // must not kill the editor); binary stdio and a UTF-8 console on Windows.
  os_init();

  splash_screen = gcalloc(PATH_MAX, sizeof(Utf8Char));
  active = NULL;
  utf8Printf(splash_screen, PATH_MAX, "%s/assets/beluga.txt", utilBelugaDir());

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
  // Everything in init.lisp (and every package) has loaded and the
  // startup buffers are open: a hook can bind keys to functions defined
  // anywhere in the config, and act on the current buffer.
  runHooks("after-init", lisp_null());
  // After the HELP line, so a config mistake is what the status bar shows
  // (a load error over an invalid setting).
  settingsReportProblems();
  configShowErrors();

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
