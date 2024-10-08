#ifndef ROW_OP_H_
#define ROW_OP_H_

#include "data.h"
#include "define.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int editorRowCxToRx(erow *row, int cursor_x);

void editorUpdateRow(erow *row);

void editorAppendRow(struct editorConfig *E, char *s, size_t len);

void editorRowInsertChar(erow *row, int at, int c);

#endif // ROW_OP_H_
