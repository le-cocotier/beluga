#include "../include/builtins.h"
#include "../include/define.h"
#include "../include/input.h"
#include "../include/file_io.h"
#include "../include/editor_op.h"
#include "../include/data.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Lisp mapKey(Lisp args, LispError *e, LispContext ctx) {
  const char *key_sequence = lisp_string(lisp_car(args));
  args = lisp_cdr(args);
  // second argument
  Lisp func = lisp_car(args);

  E.key_binds =
      (struct keyBind_t *)realloc(E.key_binds, ++E.number_of_keybinds * sizeof(struct keyBind_t));
  E.key_binds[E.number_of_keybinds - 1].key_sequence = (char *) malloc(50 * sizeof(char));

  strncpy(E.key_binds[E.number_of_keybinds - 1].key_sequence, key_sequence, 50);

  E.key_binds[E.number_of_keybinds - 1].command = func;

  return lisp_null();
}

Lisp moveCursor(Lisp args, LispError *e, LispContext ctx) {
  fprintf(stderr, "Cursor is moving\n");
  const char *direction = lisp_string(lisp_car(args));
  switch (direction[0]) {
  case 'u':
    editorMoveCursor(ARROW_UP);
    break;
  case 'd':
    editorMoveCursor(ARROW_DOWN);
    break;
  case 'r':
    editorMoveCursor(ARROW_RIGHT);
    break;
  case 'l':
    editorMoveCursor(ARROW_LEFT);
    break;
  }


  return lisp_null();
}

Lisp editorQuit(Lisp args, LispError* e, LispContext ctx) {
  if (E.dirty && E.quit_times_buffer > 0) {
    editorSetStatusMessage("WARNING! Changes hasn't been saved. Press Ctrl-Q "
                           "another time to quit.");
    --E.quit_times_buffer;
    return lisp_null();
  }
  write(STDOUT_FILENO, "\x1b[2J", 4);
  write(STDOUT_FILENO, CURSOR_TOP_LEFT, 3);
  disableRawMode();
  exit(0);

  return lisp_null();

  
}


Lisp l_editorSave(Lisp args, LispError* e, LispContext ctx) {

  editorSave();
  
  return lisp_null();

  
}

Lisp l_editorInsertNewLine(Lisp args, LispError* e, LispContext ctx) {

  editorInsertNewLine();
  
  return lisp_null();

  
}

Lisp moveCursorBeginLine(Lisp args, LispError *e, LispContext ctx) {
  E.cursor_x = 0;
  return lisp_null();
}

Lisp moveCursorEndLine(Lisp args, LispError* e, LispContext ctx) {
  if (E.cursor_y < E.numrows) {
    E.cursor_x = E.row[E.cursor_y].size;
  }
  return lisp_null();
}


Lisp deletePreviousChar(Lisp args, LispError* e, LispContext ctx) {
  editorDelChar();
  return lisp_null();
}

Lisp editorMoveCursorPageUp(Lisp args, LispError* e, LispContext ctx) {
  E.cursor_y = E.row_offset;
  int times = E.screenrows;
  while (--times) {
    editorMoveCursor(ARROW_UP);
  }
  return lisp_null();
}

Lisp editorMoveCursorPageDown(Lisp args, LispError* e, LispContext ctx) {
  E.cursor_y = E.row_offset + E.screenrows - 1;
  if (E.cursor_y > E.numrows) {
    E.cursor_y = E.numrows;
  }
  int times = E.screenrows;
  while (--times) {
    editorMoveCursor(ARROW_DOWN);
  }

  return lisp_null();
}

Lisp editorOpenFile(Lisp args, LispError *e, LispContext ctx) {
  char *filename = editorPrompt("Path : %s");
  if (filename)
    editorOpen(filename);

  return lisp_null();
}


Lisp editorPrintC(Lisp args, LispError *e, LispContext ctx) {
  char c = lisp_char(lisp_car(args));
  editorInsertChar(c);
  return lisp_null();
}

