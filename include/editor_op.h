#ifndef EDITOR_OP_H_
#define EDITOR_OP_H_

#include "data.h"
void editorInsertChar(struct editorConfig *E, int c);

void editorInsertNewLine(struct editorConfig *E);

void editorDelChar(struct editorConfig *E);

#endif // EDITOR_OP_H_
