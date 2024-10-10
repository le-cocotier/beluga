#include "../include/editor_op.h"
#include "../include/row_op.h"

void editorInsertChar(struct editorConfig *E, int c) {
  if (E->cursor_y == E->numrows) {
    editorInsertRow(E, E->numrows, "", 0);
  }
  editorRowInsertChar(E, &E->row[E->cursor_y], E->cursor_x, c);
  E->cursor_x++;
}

void editorInsertNewLine(struct editorConfig *E) {
  erow *row;
  if (!E->cursor_x) {
    editorInsertRow(E, E->cursor_y, "", 0);
  } else {
    row = &E->row[E->cursor_y];
    editorInsertRow(E, E->cursor_y + 1, &row->chars[E->cursor_x],
                    row->size - E->cursor_x);
    row = &E->row[E->cursor_y];
    row->size = E->cursor_x;
    row->chars[row->size] = '\0';
    editorUpdateRow(row);
  }
  ++E->cursor_y;
  E->cursor_x = 0;
}

void editorDelChar(struct editorConfig *E) {
  erow *row;
  if (E->cursor_y == E->numrows || !(E->cursor_x || E->cursor_y)) {
    return;
  }
  row = &E->row[E->cursor_y];
  if (E->cursor_x > 0) {
    editorRowDelchar(E, row, E->cursor_x - 1);
    --E->cursor_x;
  } else {
    E->cursor_x = E->row[E->cursor_y - 1].size;
    editorRowAppendString(E, &E->row[E->cursor_y - 1], row->chars, row->size);
    editorDelRow(E, E->cursor_y);
    --E->cursor_y;
  }
}
