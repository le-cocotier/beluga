#include "../include/editor_op.h"
#include "../include/row_op.h"
#include "data.h"


extern struct editorConfig E;

void editorInsertChar(int c) {
    if (E.cursor_y == E.numrows) {
      editorInsertRow(E.numrows, "", 0);
    }
    editorRowInsertChar(&E.row[E.cursor_y], E.cursor_x, c);
    E.cursor_x++;
}

void editorInsertNewLine() {
  erow *row;
  if (!E.cursor_x) {
    editorInsertRow(E.cursor_y, "", 0);
  } else {
    row = &E.row[E.cursor_y];
    editorInsertRow(E.cursor_y + 1, &row->chars[E.cursor_x],
                    row->size - E.cursor_x);
    row = &E.row[E.cursor_y];
    row->size = E.cursor_x;
    row->chars[row->size] = '\0';
    editorUpdateRow(row);
  }
  ++E.cursor_y;
  E.cursor_x = 0;
}

void editorDelChar() {
  erow *row;
  if (E.cursor_y == E.numrows || !(E.cursor_x || E.cursor_y)) {
    return;
  }
  row = &E.row[E.cursor_y];
  if (E.cursor_x > 0) {
    editorRowDelchar(row, E.cursor_x - 1);
    --E.cursor_x;
  } else {
    E.cursor_x = E.row[E.cursor_y - 1].size;
    editorRowAppendString(&E.row[E.cursor_y - 1], row->chars, row->size);
    editorDelRow(E.cursor_y);
    --E.cursor_y;
  }
}
