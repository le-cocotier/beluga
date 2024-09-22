/**
 * \file main.c
 * \author Arthur Barraux
 * \brief Base file of Beluga text editor. Contain fonctions for terminal
 * interactions. \version 0.1 \date 21 septembre 2024
 */

#include <asm-generic/ioctls.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* defines */
#define BELUGA_VERSION "0.1"

#define CTRL_KEY(k) ((k) & 0x1f)

#define CURSOR_TOP_LEFT "\x1b[H"
#define HIDE_CURSOR "\x1b[?25l"
#define SHOW_CURSOR "\x1b[?25h"
#define ERASE_END_LINE "\x1b[K"

#define ABUF_INIT {NULL, 0}

enum editorKey {
  CURSOR_UP = 1000,
  CURSOR_DOWN,
  CURSOR_LEFT,
  CURSOR_RIGHT,
  PAGE_UP,
  PAGE_DOWN
};

/* data */
/**
 * \struct editorConfig
 * \brief Containing our editor state.
 */
struct editorConfig {
  int cursor_x, cursor_y;
  int screenrows;
  int screencols;
  struct termios orig_termios;
};

struct abuf {
  char *b;
  int len;
};

struct editorConfig E;

/* terminal */

/**
 * \fn void die(const char *s)
 * \brief Exit program and return s error message.
 * \param *s Error string
 * */
void die(const char *s) {
  write(STDOUT_FILENO, "\x1b[2J", 4);
  write(STDOUT_FILENO, CURSOR_TOP_LEFT, 3);
  perror(s);
  exit(1);
}

void disableRawMode() {
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1) {
    die("tcsetattr");
  }
}

void enableRawMode() {
  if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) {
    die("tcgetattr");
  }
  atexit(disableRawMode);

  struct termios raw = E.orig_termios;
  raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  raw.c_oflag &= ~(OPOST);
  raw.c_cflag |= (CS8);
  raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 1;

  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
    die("tcgetattr");
  }
}

int editorReadKey() {
  int nread;
  char c;
  char seq[3];
  while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
    if (nread == -1 && errno != EAGAIN) {
      die("read");
    }
  }

  if (c == '\x1b') {
    if (read(STDIN_FILENO, &seq[0], 1) != 1 ||
        read(STDIN_FILENO, &seq[1], 1) != 1) {
      return '\x1b';
    }
    if (seq[0] == '[') {
      if (seq[1] >= '0' && seq[1] <= '9') {
        if (read(STDIN_FILENO, &seq[2], 1) != 1) {
          return '\x1b';
        }
        if (seq[2] == '~') {
          switch (seq[1]) {
          case '5':
            return PAGE_UP;
          case '6':
            return PAGE_DOWN;
          }
        }
      } else {

        switch (seq[1]) {
        case 'A':
          return CURSOR_UP;
        case 'B':
          return CURSOR_DOWN;
        case 'C':
          return CURSOR_LEFT;
        case 'D':
          return CURSOR_RIGHT;
        }
      }
    }
    return '\x1b';
  } else {
    return c;
  }
}

int getCursorPosition(int *rows, int *cols) {
  char buf[32];
  unsigned int i = 0;

  if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) {
    return -1;
  }

  while (i < sizeof(buf) - 1) {
    if (read(STDIN_FILENO, &buf[i], 1) != 1) {
      break;
    }
    if (buf[i] == 'R') {
      break;
    }
    ++i;
  }
  buf[i] = '\0';

  if (buf[0] != '\x1b' || buf[1] != '[') {
    return -1;
  }
  if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) {
    return -1;
  }

  return 0;
}

int getWindowSize(int *rows, int *cols) {
  struct winsize ws;

  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
    if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) {
      return -1;
    }
    return getCursorPosition(rows, cols);
  } else {
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
  }
}

/* append buffer */
void abAppend(struct abuf *ab, const char *s, int len) {
  char *new = realloc(ab->b, ab->len + len);

  if (new == NULL) {
    return;
  }
  memcpy(&new[ab->len], s, len);
  ab->b = new;
  ab->len += len;
}

void abFree(struct abuf *ab) { free(ab->b); }

/* output */

/**
 * \fn void editorDrawRows(struct abuf *ab)
 * \brief Draws left rows of the editor.
 */
void editorDrawRows(struct abuf *ab) {
  int y;
  char welcome[80];
  int welcome_len;
  int padding;
  for (y = 0; y < E.screenrows; ++y) {
    if (y == E.screenrows / 3) {
      welcome_len =
          snprintf(welcome, sizeof(welcome), "Beluga text editor -- version %s",
                   BELUGA_VERSION);
      if (welcome_len > E.screencols) {
        welcome_len = E.screencols;
      }
      padding = (E.screencols - welcome_len) / 2;
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
    abAppend(ab, ERASE_END_LINE, 3);
    if (y < E.screenrows - 1) {
      abAppend(ab, "\r\n", 2);
    }
  }
}

void editorRefreshScreen() {
  struct abuf ab = ABUF_INIT;
  char buf[32];

  abAppend(&ab, HIDE_CURSOR, 6);
  abAppend(&ab, CURSOR_TOP_LEFT, 3);

  editorDrawRows(&ab);

  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cursor_y + 1, E.cursor_x + 1);
  abAppend(&ab, buf, strlen(buf));

  abAppend(&ab, SHOW_CURSOR, 6);

  write(STDOUT_FILENO, ab.b, ab.len);
  abFree(&ab);
}

/* input */

/**
 * \fn void editorProcessKeypress()
 * \brief Get the last key input and do the proper action.
 */

void editorMoveCursor(int key) {
  switch (key) {
  case CURSOR_LEFT:
    if (E.cursor_x != 0) {
      --E.cursor_x;
    }
    break;
  case CURSOR_DOWN:
    if (E.cursor_x != E.screencols - 1) {
      ++E.cursor_y;
    }
    break;
  case CURSOR_UP:
    if (E.cursor_y != 0) {
      --E.cursor_y;
    }
    break;
  case CURSOR_RIGHT:
    if (E.cursor_y != E.screenrows - 1) {
      ++E.cursor_x;
    }
    break;
  }
}

void editorProcessKeypress() {
  int c = editorReadKey();
  int times;

  switch (c) {
  case CTRL_KEY('q'):
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, CURSOR_TOP_LEFT, 3);
    exit(0);
    break;

  case PAGE_UP:
  case PAGE_DOWN: {
    times = E.screenrows;
    while (--times) {
      editorMoveCursor(c == PAGE_UP ? CURSOR_UP : CURSOR_DOWN);
    }
  } break;

  case CURSOR_UP:
  case CURSOR_DOWN:
  case CURSOR_LEFT:
  case CURSOR_RIGHT:
    editorMoveCursor(c);
    break;
  }
}

/* init */

/**
 * \fn void initEditor()
 * \brief Job's function is to initialize all the fields of editorConfig.
 * */
void initEditor() {
  E.cursor_x = 0;
  E.cursor_y = 0;
  if (getWindowSize(&E.screenrows, &E.screencols) == -1) {
    die("getWindowSize");
  }
}

int main() {

  enableRawMode();
  initEditor();

  while (1) {
    editorRefreshScreen();
    editorProcessKeypress();
  }
  return 0;
}
