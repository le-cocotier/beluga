#include "../include/row_op.h"
#include <stdlib.h>
#include <string.h>

int editorRowCxToRx(erow *row, int cursor_x) {
  int render_x = 0;
  int i;
  for (i = 0; i < cursor_x; ++i) {
    if (row->chars[i] == '\t') {
      render_x += (TAB_LENGTH - 1) - (render_x % TAB_LENGTH);
    }
    render_x++;
  }
  return render_x;
}

/**
 * \fn editorUpdateRow(erow *row)
 * \brief Copy content of \p row in \p row->render.
 *  */

void editorUpdateRow(erow *row) {
  int i, i_render;
  int tabs = 0;

  // counting number of tabs

  for (i = 0; i < row->size; ++i) {
    tabs +=
        (row->chars[i] == '\t'); /**< increment tabs of 1 if chars[i] is one. */
  }

  free(row->render);
  row->render = malloc(row->size + tabs * (TAB_LENGTH - 1) +
                       1); /**< Tabs needs TAB_LENGTH chars so TAB_LENGTH - 1
                              more than the first already counted. */

  // end of counting
  i_render = 0;
  for (i = 0; i < row->size; ++i) {
    if (row->chars[i] == '\t') {
      row->render[i_render++] = ' ';
      while (i_render % TAB_LENGTH) {
        row->render[i_render++] =
            ' '; /**< Addind the right amount of spaces for tabs */
      }
    } else {
      row->render[i_render++] = row->chars[i];
    }
  }
  row->render[i_render] = '\0'; // Don't forget the end of string character.
  row->rsize = i_render;
}

void editorAppendRow(struct editorConfig *E, char *s, size_t len) {
  int at;
  E->row = realloc(E->row, sizeof(erow) * (E->numrows + 1));

  at = E->numrows;
  E->row[at].size = len;
  E->row[at].chars = malloc(len + 1);
  memcpy(E->row[at].chars, s, len);
  E->row[at].chars[len] = '\0';

  E->row[at].rsize = 0;
  E->row[at].render = NULL;
  editorUpdateRow(&E->row[at]);

  ++E->numrows;
}

/**
 * \fn editorRowInsertChar(erow *row, int at, int c)
 * \param at Index of where we want to insert the char */

void editorRowInsertChar(erow *row, int at, int c) {
  if (at < 0 || at > row->size) {
    at = row->size;
  }
  row->chars = realloc(row->chars, row->size + 2);
  memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
  ++row->size;
  row->chars[at] = c;
  editorUpdateRow(row);
}
