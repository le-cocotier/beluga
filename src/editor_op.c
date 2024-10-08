#include "../include/editor_op.h"
#include "../include/row_op.h"

void editorInsertChar(struct editorConfig *E, int c) {
  if (E->cursor_y == E->numrows) {
    editorAppendRow(E, "", 0);
  }
  editorRowInsertChar(&E->row[E->cursor_y], E->cursor_x, c);
  E->cursor_x++;
}
