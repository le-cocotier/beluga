#include "../include/init.h"

void initEditor(struct editorConfig *E) {
  E->cursor_x = 0;
  E->cursor_y = 0;
  E->row_offset = 0;
  E->numrows = 0;
  E->row = NULL;
  if (getWindowSize(&E->screenrows, &E->screencols) == -1) {
    die("getWindowSize");
  }
}
