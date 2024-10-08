#include "../include/output.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

void editorDrawRows(struct editorConfig *E, struct abuf *ab) {
  int y;
  char welcome[80];
  int welcome_len;
  int padding;
  int len;
  int file_row;
  for (y = 0; y < E->screenrows; ++y) {
    file_row = y + E->row_offset;
    if (file_row >= E->numrows) {
      if (E->numrows == 0 && y == E->screenrows / 3) {
        welcome_len =
            snprintf(welcome, sizeof(welcome),
                     "Beluga text editor -- version %s", BELUGA_VERSION);
        if (welcome_len > E->screencols) {
          welcome_len = E->screencols;
        }
        padding = (E->screencols - welcome_len) / 2;
        if (padding) {
          abAppend(ab, "~", 1);
          --padding;
        }
        while (padding--) {
          abAppend(ab, " ", 1);
        }
        abAppend(ab, welcome, welcome_len);
      } else {
        abAppend(ab, "~", 1);
      }
    } else {
      len = E->row[file_row].rsize - E->col_offset;
      if (len < 0)
        len = 0;
      if (len > E->screencols)
        len = E->screencols;
      abAppend(ab, &E->row[file_row].render[E->col_offset], len);
    }
    abAppend(ab, ERASE_END_LINE, 3);
    abAppend(ab, "\r\n", 2);
  }
}

void editorScroll(struct editorConfig *E) {
  E->rx = E->cursor_x;
  if (E->cursor_y < E->numrows) {
    E->rx = editorRowCxToRx(&E->row[E->cursor_y], E->cursor_x);
  }

  if (E->cursor_y < E->row_offset) {
    E->row_offset = E->cursor_y;
  }
  if (E->cursor_y >= E->row_offset + E->screenrows) {
    E->row_offset = E->cursor_y - E->screenrows + 1;
  }
  if (E->rx < E->col_offset) {
    E->col_offset = E->rx;
  }
  if (E->rx >= E->col_offset + E->screencols) {
    E->col_offset = E->rx - E->screencols + 1;
  }
}

void editorDrawStatusBar(struct editorConfig *E, struct abuf *ab) {
  int len, render_len;
  char status[80], render_status[80];

  abAppend(ab, "\x1b[7m", 4); // inverting colors
  len = snprintf(status, sizeof(status), "%.20s - %d lines",
                 E->filename ? E->filename : "[No Name]", E->numrows);
  render_len = snprintf(render_status, sizeof(render_status), "%d/%d",
                        E->cursor_y + 1, E->numrows);
  if (len > E->screencols) {
    len = E->screencols;
  }
  abAppend(ab, status, len);
  while (len < E->screencols) {
    if (E->screencols - len == render_len) {
      abAppend(ab, render_status, render_len);
      break;
    } else {
      abAppend(ab, " ", 1);
      ++len;
    }
  }
  abAppend(ab, "\x1b[m", 3); // normal text mode
  abAppend(ab, "\r\n", 2);
}

void editorDrawMessageBar(struct editorConfig *E, struct abuf *ab) {
  int msg_len = strlen(E->status_msg);
  abAppend(ab, ERASE_END_LINE, 3);
  if (msg_len > E->screencols) {
    msg_len = E->screencols;
  }
  if (msg_len && time(NULL) - E->status_msg_time < 5) {
    abAppend(ab, E->status_msg, msg_len);
  }
}

void editorRefreshScreen(struct editorConfig *E) {
  editorScroll(E);
  struct abuf ab = ABUF_INIT;
  char buf[32];

  abAppend(&ab, HIDE_CURSOR, 6);
  abAppend(&ab, CURSOR_TOP_LEFT, 3);

  editorDrawRows(E, &ab);
  editorDrawStatusBar(E, &ab);
  editorDrawMessageBar(E, &ab);

  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E->cursor_y - E->row_offset) + 1,
           (E->rx - E->col_offset) + 1);
  abAppend(&ab, buf, strlen(buf));

  abAppend(&ab, SHOW_CURSOR, 6);

  write(STDOUT_FILENO, ab.b, ab.len);
  abFree(&ab);
}

void editorSetStatusMessage(struct editorConfig *E, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(E->status_msg, sizeof(E->status_msg), fmt, ap);
  va_end(ap);
  E->status_msg_time = time(NULL);
}
