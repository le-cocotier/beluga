#include "../include/input.h"

void editorMoveCursor(struct editorConfig *E, int key) {
  switch (key) {
  case CURSOR_RIGHT:
    if (E->cursor_x != 0) {
      --E->cursor_x;
    }
    break;
  case CURSOR_DOWN:
    if (E->cursor_y != E->screenrows - 1) {
      ++E->cursor_y;
    }
    break;
  case CURSOR_UP:
    if (E->cursor_y != 0) {
      --E->cursor_y;
    }
    break;
  case CURSOR_LEFT:
    if (E->cursor_x != E->screencols - 1) {
      ++E->cursor_x;
    }
    break;
  }
}

void editorProcessKeypress(struct editorConfig *E) {
  int c = editorReadKey();
  int times;

  switch (c) {
  case CTRL_KEY('q'):
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, CURSOR_TOP_LEFT, 3);
    disableRawMode(E);
    exit(0);
    break;

  case BEG_LINE:
    E->cursor_x = 0;
    break;

  case END_LINE:
    E->cursor_x = E->screencols - 1;
    break;

  case PAGE_UP:
  case PAGE_DOWN: {
    times = E->screenrows;
    while (--times) {
      editorMoveCursor(E, c == PAGE_UP ? CURSOR_UP : CURSOR_DOWN);
    }
  } break;

  case CURSOR_UP:
  case CURSOR_DOWN:
  case CURSOR_LEFT:
  case CURSOR_RIGHT:
    editorMoveCursor(E, c);
    break;
  }
}
