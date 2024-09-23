#include "../include/row_op.h"

void editorAppendRow(struct editorConfig *E, char *s, size_t len) {
  int at;
  E->row = realloc(E->row, sizeof(erow) * (E->numrows + 1));

  at = E->numrows;
  E->row[at].size = len;
  E->row[at].chars = malloc(len + 1);
  memcpy(E->row[at].chars, s, len);
  E->row[at].chars[len] = '\0';
  ++E->numrows;
}
