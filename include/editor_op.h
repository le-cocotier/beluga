#ifndef EDITOR_OP_H_
#define EDITOR_OP_H_

#include "data.h"

void editorSetStatusMessage(const char *fmt, ...);
int editorRowCharCount(Row *row, int x);
int editorRowCxToByte(const Row *row, int cursor_x);
char *editorGetClipboard(void);
void editorSetClipboard(const char *text, int len);
void editorRunLoop(void);


#endif // EDITOR_OP_H_
