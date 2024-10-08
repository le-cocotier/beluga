#include "../include/init.h"

void initEditor(struct editorConfig *E) {
  E->cursor_x = 0;
  E->cursor_y = 0;
  E->rx = 0;
  E->row_offset = 0;
  E->col_offset = 0;
  E->numrows = 0;
  E->row = NULL;
  E->filename = NULL;
  E->status_msg[0] = '\0';
  E->status_msg_time = 0;
  if (getWindowSize(&E->screenrows, &E->screencols) == -1) {
    die("getWindowSize");
  }
  E->screenrows -= 2;
}
