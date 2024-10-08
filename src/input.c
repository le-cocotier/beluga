#include "../include/input.h"
#include "../include/editor_op.h"
#include "../include/file_io.h"
#include <stdint.h>

void editorMoveCursor(struct editorConfig *E, int key) {
  erow *row = (E->cursor_y >= E->numrows) ? NULL : &E->row[E->cursor_y];
  int row_len;
  switch (key) {
  case CURSOR_RIGHT:
    if (row && E->cursor_x < row->size) {
      ++E->cursor_x;
    } else if (row && E->cursor_x == row->size) {
      E->cursor_y++;
      E->cursor_x = 0;
    }
    break;
  case CURSOR_DOWN:
    if (E->cursor_y < E->numrows) {
      ++E->cursor_y;
    }
    break;
  case CURSOR_UP:
    if (E->cursor_y != 0) {
      --E->cursor_y;
    }
    break;
  case CURSOR_LEFT:
    if (E->cursor_x != 0) {
      --E->cursor_x;
    } else if (E->cursor_y > 0) {
      --E->cursor_y;
      E->cursor_x = E->row[E->cursor_y].size;
    }
    break;
  }

  row = (E->cursor_y >= E->numrows) ? NULL : &E->row[E->cursor_y];
  row_len = row ? row->size : 0;
  if (E->cursor_x > row_len) {
    E->cursor_x = row_len;
  }
}

void editorProcessKeypress(struct editorConfig *E) {
  int c = editorReadKey();
  int times;

  switch (c) {
  case '\r':
    /* TODO */
    break;
  case CTRL_KEY('q'):
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, CURSOR_TOP_LEFT, 3);
    disableRawMode(E);
    exit(0);
    break;

  case CTRL_KEY('s'):
    editorSave(E);
    break;

  case BEG_LINE:
    E->cursor_x = 0;
    break;

  case END_LINE:
    if (E->cursor_y < E->numrows) {
      E->cursor_x = E->row[E->cursor_y].size;
    }
    break;

  case BACKSPACE:
  case CTRL_KEY('h'):
    // case DEL_KEY:
    /* TODO */
    break;

  case PAGE_UP:
  case PAGE_DOWN: {
    if (c == PAGE_UP) {
      E->cursor_y = E->row_offset;
    } else if (c == PAGE_DOWN) {
      E->cursor_y = E->row_offset + E->screenrows - 1;
      if (E->cursor_y > E->numrows) {
        E->cursor_y = E->numrows;
      }
    }
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

  case CTRL_KEY('l'):
  case '\x1b':
    break;
  default:
    editorInsertChar(E, c);
    break;
  }
}
