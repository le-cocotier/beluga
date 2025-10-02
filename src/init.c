#include "../include/init.h"
#include "../include/data.h"
#include "../include/terminal.h"
#include "../include/builtins.h"
#include <stdio.h>

#define LISP_IMPLEMENTATION
#include "../lisp-interpreter/dist/lisp.h"
#include "../lisp-interpreter/dist/lisp_lib.h"

extern struct editorConfig;


void registerBuiltin(char *key_sequence, LispCFunc f) {
  lisp_env_define(E.ctx.p->env, lisp_make_symbol(key_sequence, E.ctx),
                  lisp_make_func(f), E.ctx);
  
}

void initBuiltins() {
  // move cursor
  registerBuiltin("MOVE-CURSOR", moveCursor);
  registerBuiltin("MAP-KEY", mapKey);
  registerBuiltin("EDITOR-QUIT", editorQuit);
  registerBuiltin("EDITOR-SAVE", l_editorSave);
  registerBuiltin("EDITOR-INSERT-NEW-LINE", l_editorInsertNewLine);
  registerBuiltin("MOVE-CURSOR-BEG-LINE", moveCursorBeginLine);
  registerBuiltin("MOVE-CURSOR-END-LINE", moveCursorEndLine);
  registerBuiltin("EDITOR-DELETE-PREVIOUS-CHAR", deletePreviousChar);
  registerBuiltin("MOVE-CURSOR-PAGE-UP", editorMoveCursorPageUp);
  registerBuiltin("MOVE-CURSOR-PAGE-DOWN", editorMoveCursorPageDown);
}

void initEditor() {
  E.cursor_x = 0;
  E.cursor_y = 0;
  E.rx = 0;
  E.row_offset = 0;
  E.col_offset = 0;
  E.numrows = 0;
  E.row = NULL;
  E.dirty = 0;
  E.filename = NULL;
  E.status_msg[0] = '\0';
  E.status_msg_time = 0;
  if (getWindowSize(&E.screenrows, &E.screencols) == -1) {
    die("getWindowSize");
  }
  E.screenrows -= 2;

  E.number_of_keybinds = 0;

  
  E.fd_init_file = fopen("config/init.lisp", "r");
  E.ctx = lisp_init();
  E.env = lisp_env(E.ctx);
  lisp_lib_load(E.ctx);
  // Init builtins lisp functions
  initBuiltins();

  // Read config file
  E.ctx_data = lisp_read_file(E.fd_init_file, &E.ctx_error, E.ctx);
  if (E.ctx_error != LISP_ERROR_NONE) {
    die("init failed");
  }
  lisp_eval(E.ctx_data, &E.ctx_error, E.ctx);
  
  // To modify

  E.constantes.TAB_LENGTH =
      (int)lisp_eval(lisp_read("TAB-LENGTH", &E.ctx_error, E.ctx), &E.ctx_error,
                     E.ctx)
          .val.int_val;
  E.constantes.QUIT_TIMES =
      (int)lisp_eval(lisp_read("QUIT-TIMES", &E.ctx_error, E.ctx), &E.ctx_error,
                     E.ctx)
          .val.int_val;

  E.quit_times_buffer = E.constantes.QUIT_TIMES;
}
