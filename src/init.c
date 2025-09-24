#include "../include/init.h"
#include "../include/data.h"
#include <stdio.h>

extern struct editorConfig;

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

  // E.fd_init_file = fopen("../config/init.el", "r");

  // E.ctx = lisp_init();
  // E.ctx_data = lisp_read_file(E.fd_init_file, &E.ctx_error, E.ctx);
  // lisp_printf(stderr, E.ctx_data);
}
