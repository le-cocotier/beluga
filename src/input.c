#include "../include/input.h"
#include "../include/editor_op.h"
#include "../include/file_io.h"
#include "../include/output.h"
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/**
 * \fn char * editorPrompt(struct editorConfig *E, char *prompt)
 * \brief Return user input in a prompt when enter is hit. */

char *editorPrompt(struct editorConfig *E, char *prompt) {
  size_t buf_size = 128;
  char *buf = malloc(buf_size);
  size_t buf_len = 0;
  int c = 0;
  buf[0] = '\0';

  while (1) {
    editorSetStatusMessage(E, prompt, buf);
    editorRefreshScreen(E);
    c = editorReadKey();
    if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE) {
      if (buf_len != 0) {
        buf[--buf_len] = '\0';
      }
    } else if (c == ESCAPE) {
      fprintf(stderr, "escape");
      editorSetStatusMessage(E, "");
      free(buf);
      return NULL;
    } else if (c == '\r') {
      if (buf_len != 0) {
        editorSetStatusMessage(E, "");
        return buf;
      }
    } else if (!iscntrl(c) && c < 128) {
      if (buf_len == buf_size - 1) {
        buf_size *= 2;
        buf = realloc(buf, buf_size);
      }
      buf[buf_len++] = c;
      buf[buf_len] = '\0';
    }
  }
}

void editorMoveCursor(struct editorConfig *E, int key) {
  erow *row = (E->cursor_y >= E->numrows) ? NULL : &E->row[E->cursor_y];
  int row_len;
  switch (key) {
  case ARROW_RIGHT:
    if (row && E->cursor_x < row->size) {
      ++E->cursor_x;
    } else if (row && E->cursor_x == row->size) {
      E->cursor_y++;
      E->cursor_x = 0;
    }
    break;
  case ARROW_DOWN:
    if (E->cursor_y < E->numrows) {
      ++E->cursor_y;
    }
    break;
  case ARROW_UP:
    if (E->cursor_y != 0) {
      --E->cursor_y;
    }
    break;
  case ARROW_LEFT:
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
  static int quit_times = QUIT_TIMES;
  int c = editorReadKey();
  int times;

  switch (c) {

  case '\r':
    editorInsertNewLine(E);
    break;
  case CTRL_KEY('q'):
    if (E->dirty && quit_times > 0) {
      editorSetStatusMessage(E,
                             "WARNING! Changes hasn't been saved. Press Ctrl-Q "
                             "another time to quit.");
      --quit_times;
      return;
    }
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
  case DEL_KEY:
    if (c == DEL_KEY) {
      editorMoveCursor(E, ARROW_RIGHT);
    }
    editorDelChar(E);
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
      editorMoveCursor(E, c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
    }
  } break;

  case ARROW_UP:
  case ARROW_DOWN:
  case ARROW_LEFT:
  case ARROW_RIGHT:
    editorMoveCursor(E, c);
    break;

  case CTRL_KEY('l'):
  case '\x1b':
    break;
  default:
    editorInsertChar(E, c);
    break;
  }
  quit_times = QUIT_TIMES;
}
