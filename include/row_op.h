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

void editorInsertRow(int at, char *s, size_t len);

void editorFreeRow(erow *row);

void editorDelRow(int at);

void editorRowInsertChar(erow *row, int at, int c);

void editorRowAppendString(erow *row, char *s, size_t len);

void editorRowDelchar(erow *row, int at);

#endif // ROW_OP_H_
