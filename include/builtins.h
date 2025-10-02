#ifndef BUILTINS_H_
#define BUILTINS_H_

#include "../lisp-interpreter/dist/lisp.h"

Lisp moveCursor(Lisp args, LispError *e, LispContext ctx);

Lisp mapKey(Lisp args, LispError *e, LispContext ctx);

void registerBuiltin(char * key_sequence, LispCFunc f);

Lisp editorQuit(Lisp args, LispError *e, LispContext ctx);

Lisp l_editorSave(Lisp args, LispError *e, LispContext ctx);

Lisp l_editorInsertNewLine(Lisp args, LispError* e, LispContext ctx);

Lisp moveCursorBeginLine(Lisp args, LispError *e, LispContext ctx);

Lisp moveCursorEndLine(Lisp args, LispError* e, LispContext ctx);

#endif
