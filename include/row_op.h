#ifndef ROW_OP_H_
#define ROW_OP_H_

#include "data.h"
#include "define.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int editorRowCxToRx(erow *row, int cursor_x);

void editorUpdateRow(erow *row);

void editorInsertRow(struct editorConfig *E, int at, char *s, size_t len);

void editorFreeRow(erow *row);

void editorDelRow(struct editorConfig *E, int at);

void editorRowInsertChar(struct editorConfig *E, erow *row, int at, int c);

void editorRowAppendString(struct editorConfig *E, erow *row, char *s,
                           size_t len);

void editorRowDelchar(struct editorConfig *E, erow *row, int at);

#endif // ROW_OP_H_
