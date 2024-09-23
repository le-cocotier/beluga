#ifndef DATA_H_
#define DATA_H_

#include <termios.h>

typedef struct erow {
  int size;
  char *chars;
} erow;

/**
 * \struct editorConfig
 * \brief Containing our editor state.
 */
struct editorConfig {
  int cursor_x, cursor_y;
  int row_offset;
  int screenrows;
  int screencols;
  int numrows;
  erow *row;
  struct termios orig_termios;
};

struct abuf {
  char *b;
  int len;
};

#endif
