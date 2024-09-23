#include "../include/output.h"

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
      len = E->row[file_row].size > E->screencols ? E->screencols
                                                  : E->row[y].size;
      abAppend(ab, E->row[file_row].chars, len);
    }
    abAppend(ab, ERASE_END_LINE, 3);
    if (y < E->screenrows - 1) {
      abAppend(ab, "\r\n", 2);
    }
  }
}

void editorRefreshScreen(struct editorConfig *E) {
  struct abuf ab = ABUF_INIT;
  char buf[32];

  abAppend(&ab, HIDE_CURSOR, 6);
  abAppend(&ab, CURSOR_TOP_LEFT, 3);

  editorDrawRows(E, &ab);

  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E->cursor_y + 1, E->cursor_x + 1);
  abAppend(&ab, buf, strlen(buf));

  abAppend(&ab, SHOW_CURSOR, 6);

  write(STDOUT_FILENO, ab.b, ab.len);
  abFree(&ab);
}
